#include "kdmapper.hpp"
#include "skCrypter.h"

static int loadedimports = 0;

uintptr_t export_import(HANDLE handle, const char* export_name)
{
	const auto address = intel_driver::GetKernelModuleExport(handle, utils::GetKernelModuleAddress("ntoskrnl.exe"), export_name);
	if (!address)
	{
		printf("[!] %s has failed to export, exiting.\n", export_name);
		Sleep(3500);
		exit(-5);
	}

	loadedimports++;

	return address;
}



uint64_t kdmapper::AllocMdlMemory(HANDLE iqvw64e_device_handle, uint64_t size, uint64_t* mdlPtr) {
	/*added by psec*/
	LARGE_INTEGER LowAddress, HighAddress;
	LowAddress.QuadPart = 0;
	HighAddress.QuadPart = 0xffff'ffff'ffff'ffffULL;

	uint64_t pages = (size / PAGE_SIZE) + 1;
	auto mdl = intel_driver::MmAllocatePagesForMdl(iqvw64e_device_handle, LowAddress, HighAddress, LowAddress, pages * (uint64_t)PAGE_SIZE);
	if (!mdl) {
		return { 0 };
	}

	uint32_t byteCount = 0;
	if (!intel_driver::ReadMemory(iqvw64e_device_handle, mdl + 0x028 /*_MDL : byteCount*/, &byteCount, sizeof(uint32_t))) {
		return { 0 };
	}

	if (byteCount < size) {
		intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdl);
		intel_driver::FreePool(iqvw64e_device_handle, mdl);
		return { 0 };
	}

	auto mappingStartAddress = intel_driver::MmMapLockedPagesSpecifyCache(iqvw64e_device_handle, mdl, nt::KernelMode, nt::MmCached, NULL, FALSE, nt::NormalPagePriority);
	if (!mappingStartAddress) {
		intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdl);
		intel_driver::FreePool(iqvw64e_device_handle, mdl);
		return { 0 };
	}

	const auto result = intel_driver::MmProtectMdlSystemAddress(iqvw64e_device_handle, mdl, PAGE_EXECUTE_READWRITE);
	if (!result) {
		intel_driver::MmUnmapLockedPages(iqvw64e_device_handle, mappingStartAddress, mdl);
		intel_driver::MmFreePagesFromMdl(iqvw64e_device_handle, mdl);
		intel_driver::FreePool(iqvw64e_device_handle, mdl);
		return { 0 };
	}

	if (mdlPtr)
		*mdlPtr = mdl;

	return mappingStartAddress;
}

#include <rpcdce.h>

uint64_t kdmapper::MapDriver( HANDLE iqvw64e_device_handle, BYTE* data, ULONG64 param1, ULONG64 param2, bool free, bool destroyHeader, bool mdlMode, bool PassAllocationAddressAsFirstParam, mapCallback callback, NTSTATUS* exitCode ) {

	const PIMAGE_NT_HEADERS64 nt_headers = portable_executable::GetNtHeaders( data );

	if ( !nt_headers ) {
		Log( L"[-] Invalid format of PE image" << std::endl );
		return 0;
	}

	if ( nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ) {
		Log( L"[-] Image is not 64 bit" << std::endl );
		return 0;
	}

	uint32_t image_size = nt_headers->OptionalHeader.SizeOfImage;

	void* local_image_base = VirtualAlloc( nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
	if ( !local_image_base )
		return 0;

	DWORD TotalVirtualHeaderSize = ( IMAGE_FIRST_SECTION( nt_headers ) )->VirtualAddress;
	image_size = image_size - ( destroyHeader ? TotalVirtualHeaderSize : 0 );

	uint64_t kernel_image_base = 0;
	uint64_t mdlptr = 0;
	if ( mdlMode ) {
		kernel_image_base = AllocMdlMemory( iqvw64e_device_handle, image_size, &mdlptr );
	}
	else {
		kernel_image_base = intel_driver::AllocatePool( iqvw64e_device_handle, nt::POOL_TYPE::NonPagedPool, image_size );
	}

	do {
		if ( !kernel_image_base ) {
			Log( L"[-] Failed to allocate remote image in kernel" << std::endl );
			break;
		}

		Log( L"[+] Image base has been allocated at 0x" << reinterpret_cast< void* >( kernel_image_base ) << std::endl );

		// Copy image headers

		memcpy( local_image_base, data, nt_headers->OptionalHeader.SizeOfHeaders );

		// Copy image sections

		const PIMAGE_SECTION_HEADER current_image_section = IMAGE_FIRST_SECTION( nt_headers );

		for ( auto i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i ) {
			if ( ( current_image_section [ i ].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA ) > 0 )
				continue;
			auto local_section = reinterpret_cast< void* >( reinterpret_cast< uint64_t >( local_image_base ) + current_image_section [ i ].VirtualAddress );
			memcpy( local_section, reinterpret_cast< void* >( reinterpret_cast< uint64_t >( data ) + current_image_section [ i ].PointerToRawData ), current_image_section [ i ].SizeOfRawData );
		}

		uint64_t realBase = kernel_image_base;
		if ( destroyHeader ) {
			kernel_image_base -= TotalVirtualHeaderSize;
			Log( L"[+] Skipped 0x" << std::hex << TotalVirtualHeaderSize << L" bytes of PE Header" << std::endl );
		}

		// Resolve relocs and imports

		RelocateImageByDelta( portable_executable::GetRelocs( local_image_base ), kernel_image_base - nt_headers->OptionalHeader.ImageBase );

		if ( !ResolveImports( iqvw64e_device_handle, portable_executable::GetImports( local_image_base ) ) ) {
			Log( L"[-] Failed to resolve imports" << std::endl );
			kernel_image_base = realBase;
			break;
		}

		// Write fixed image to kernel

		if ( !intel_driver::WriteMemory( iqvw64e_device_handle, realBase, ( PVOID ) ( ( uintptr_t ) local_image_base + ( destroyHeader ? TotalVirtualHeaderSize : 0 ) ), image_size ) ) {
			Log( L"[-] Failed to write local image to remote image" << std::endl );
			kernel_image_base = realBase;
			break;
		}

		// Call driver entry point

		const uint64_t address_of_entry_point = kernel_image_base + nt_headers->OptionalHeader.AddressOfEntryPoint;

		Log( L"[<] Calling DriverEntry 0x" << reinterpret_cast< void* >( address_of_entry_point ) << std::endl );

		struct
		{
			//uintptr_t ex_allocate_pool;
			//uintptr_t zw_query_system_information;
			//uintptr_t ex_free_pool_with_tag;
			//uintptr_t rtl_init_ansi_string;
			//uintptr_t rtl_ansi_string_to_unicode_string;
			//uintptr_t mm_copy_virtual_memory;
			//uintptr_t io_get_current_process;
			//uintptr_t ps_lookup_process_by_process_id;
			//uintptr_t ps_get_process_peb;
			//uintptr_t ob_reference_object_safe;
			//uintptr_t rtl_compare_unicode_string;
			//uintptr_t rtl_free_unicode_string;
			//uintptr_t obf_dereference_object;
			//uintptr_t mm_copy_memory;
			//uintptr_t ps_get_process_section_base_address;
			//uintptr_t io_create_driver;
			//uintptr_t io_allocate_mdl;
			//uintptr_t mm_probe_and_lock_pages;
			//uintptr_t mm_map_locked_pages_specify_cache;
			//uintptr_t mm_protect_mdl_system_address;
			//uintptr_t mm_unmap_locked_pages;
			//uintptr_t mm_unlock_pages;
			//uintptr_t io_free_mdl;
			//uintptr_t iof_complete_request;
			//uintptr_t rtl_init_unicode_string;
			//uintptr_t io_create_symbolic_link;
			//uintptr_t io_delete_device;
			//uintptr_t io_create_device;
			//uintptr_t rtl_get_version;
			//uintptr_t mm_map_io_space_ex;
			//uintptr_t mm_unmap_io_space;
			//uintptr_t mm_get_virtual_for_physical;
			//uintptr_t mm_get_physical_memory_ranges;
			uintptr_t ex_allocate_pool;
			uintptr_t zw_query_system_information;
			uintptr_t ex_free_pool_with_tag;
			uintptr_t rtl_init_ansi_string;
			uintptr_t rtl_ansi_string_to_unicode_string;
			uintptr_t mm_copy_virtual_memory;
			uintptr_t io_get_current_process;
			uintptr_t ps_lookup_process_by_process_id;
			uintptr_t ps_get_process_peb;
			uintptr_t ob_reference_object_safe;
			uintptr_t rtl_compare_unicode_string;
			uintptr_t rtl_free_unicode_string;
			uintptr_t obf_dereference_object;
			uintptr_t mm_copy_memory;
			uintptr_t ps_get_process_section_base_address;
			uintptr_t io_create_driver;
			uintptr_t io_allocate_mdl;
			uintptr_t mm_probe_and_lock_pages;
			uintptr_t mm_map_locked_pages_specify_cache;
			uintptr_t mm_protect_mdl_system_address;
			uintptr_t mm_unmap_locked_pages;
			uintptr_t mm_unlock_pages;
			uintptr_t io_free_mdl;
			uintptr_t iof_complete_request;
			uintptr_t rtl_init_unicode_string;
			uintptr_t io_create_symbolic_link;
			uintptr_t io_delete_device;
			uintptr_t io_create_device;
			uintptr_t rtl_get_version;
			uintptr_t mm_map_io_space_ex;
			uintptr_t mm_unmap_io_space;
			uintptr_t mm_get_virtual_for_physical;
			uintptr_t mm_get_physical_memory_ranges;
		} imports;

		imports.ex_allocate_pool = export_import(iqvw64e_device_handle, xorstr_("ExAllocatePool"));
		imports.zw_query_system_information = export_import(iqvw64e_device_handle, xorstr_("ZwQuerySystemInformation"));
		imports.ex_free_pool_with_tag = export_import(iqvw64e_device_handle, xorstr_("ExFreePoolWithTag"));
		imports.rtl_init_ansi_string = export_import(iqvw64e_device_handle, xorstr_("RtlInitAnsiString"));
		imports.rtl_ansi_string_to_unicode_string = export_import(iqvw64e_device_handle, xorstr_("RtlAnsiStringToUnicodeString"));
		imports.mm_copy_virtual_memory = export_import(iqvw64e_device_handle, xorstr_("MmCopyVirtualMemory"));
		imports.io_get_current_process = export_import(iqvw64e_device_handle, xorstr_("IoGetCurrentProcess"));
		imports.ps_lookup_process_by_process_id = export_import(iqvw64e_device_handle, xorstr_("PsLookupProcessByProcessId"));
		imports.ps_get_process_peb = export_import(iqvw64e_device_handle, xorstr_("PsGetProcessPeb"));
		imports.ob_reference_object_safe = export_import(iqvw64e_device_handle, xorstr_("ObReferenceObjectSafe"));
		imports.rtl_compare_unicode_string = export_import(iqvw64e_device_handle, xorstr_("RtlCompareUnicodeString"));
		imports.rtl_free_unicode_string = export_import(iqvw64e_device_handle, xorstr_("RtlFreeUnicodeString"));
		imports.obf_dereference_object = export_import(iqvw64e_device_handle, xorstr_("ObfDereferenceObject"));
		imports.mm_copy_memory = export_import(iqvw64e_device_handle, xorstr_("MmCopyMemory"));
		imports.ps_get_process_section_base_address = export_import(iqvw64e_device_handle, xorstr_("PsGetProcessSectionBaseAddress"));
		imports.io_create_driver = export_import(iqvw64e_device_handle, xorstr_("IoCreateDriver"));
		imports.io_allocate_mdl = export_import(iqvw64e_device_handle, xorstr_("IoAllocateMdl"));
		imports.mm_probe_and_lock_pages = export_import(iqvw64e_device_handle, xorstr_("MmProbeAndLockPages"));
		imports.mm_map_locked_pages_specify_cache = export_import(iqvw64e_device_handle, xorstr_("MmMapLockedPagesSpecifyCache"));
		imports.mm_protect_mdl_system_address = export_import(iqvw64e_device_handle, xorstr_("MmProtectMdlSystemAddress"));
		imports.mm_unmap_locked_pages = export_import(iqvw64e_device_handle, xorstr_("MmUnmapLockedPages"));
		imports.mm_unlock_pages = export_import(iqvw64e_device_handle, xorstr_("MmUnlockPages"));
		imports.io_free_mdl = export_import(iqvw64e_device_handle, xorstr_("IoFreeMdl"));
		imports.iof_complete_request = export_import(iqvw64e_device_handle, xorstr_("IofCompleteRequest"));
		imports.rtl_init_unicode_string = export_import(iqvw64e_device_handle, xorstr_("RtlInitUnicodeString"));
		imports.io_create_symbolic_link = export_import(iqvw64e_device_handle, xorstr_("IoCreateSymbolicLink"));
		imports.io_delete_device = export_import(iqvw64e_device_handle, xorstr_("IoDeleteDevice"));
		imports.io_create_device = export_import(iqvw64e_device_handle, xorstr_("IoCreateDevice"));
		imports.rtl_get_version = export_import(iqvw64e_device_handle, xorstr_("RtlGetVersion"));
		imports.mm_map_io_space_ex = export_import(iqvw64e_device_handle, xorstr_("MmMapIoSpaceEx"));
		imports.mm_unmap_io_space = export_import(iqvw64e_device_handle, xorstr_("MmUnmapIoSpace"));
		imports.mm_get_virtual_for_physical = export_import(iqvw64e_device_handle, xorstr_("MmGetVirtualForPhysical"));
		imports.mm_get_physical_memory_ranges = export_import(iqvw64e_device_handle, xorstr_("MmGetPhysicalMemoryRanges"));

		printf( "[<] Loaded %i imports out of 33\n", loadedimports );

		NTSTATUS status = 0;

		

		if ( !intel_driver::CallKernelFunction( iqvw64e_device_handle, &status, address_of_entry_point, imports ) ) {
			Log( L"[-] Failed to call driver entry" << std::endl );
			kernel_image_base = realBase;
			break;
		}

		if ( exitCode )
			*exitCode = status;

		if ( free && mdlMode ) {
			intel_driver::MmUnmapLockedPages( iqvw64e_device_handle, realBase, mdlptr );
			intel_driver::MmFreePagesFromMdl( iqvw64e_device_handle, mdlptr );
			intel_driver::FreePool( iqvw64e_device_handle, mdlptr );
		}
		else if ( free ) {
			intel_driver::FreePool( iqvw64e_device_handle, realBase );
		}

		printf( "[<] Operation completed successfully.\n" );


		VirtualFree( local_image_base, 0, MEM_RELEASE );
		return realBase;

	} while ( false );


	VirtualFree( local_image_base, 0, MEM_RELEASE );

	intel_driver::FreePool( iqvw64e_device_handle, kernel_image_base );

	return 0;
}

void kdmapper::RelocateImageByDelta( portable_executable::vec_relocs relocs, const uint64_t delta ) {
	for ( const auto& current_reloc : relocs ) {
		for ( auto i = 0u; i < current_reloc.count; ++i ) {
			const uint16_t type = current_reloc.item [ i ] >> 12;
			const uint16_t offset = current_reloc.item [ i ] & 0xFFF;

			if ( type == IMAGE_REL_BASED_DIR64 )
				*reinterpret_cast< uint64_t* >( current_reloc.address + offset ) += delta;
		}
	}
}

bool kdmapper::ResolveImports( HANDLE iqvw64e_device_handle, portable_executable::vec_imports imports ) {
	for ( const auto& current_import : imports ) {
		ULONG64 Module = utils::GetKernelModuleAddress( current_import.module_name );
		if ( !Module ) {
#if !defined(DISABLE_OUTPUT)
			std::cout << "[-] Dependency " << current_import.module_name << " wasn't found" << std::endl;
#endif
			return false;
		}

		for ( auto& current_function_data : current_import.function_datas ) {
			uint64_t function_address = intel_driver::GetKernelModuleExport( iqvw64e_device_handle, Module, current_function_data.name );

			if ( !function_address ) {
				//Lets try with ntoskrnl
				if ( Module != intel_driver::ntoskrnlAddr ) {
					function_address = intel_driver::GetKernelModuleExport( iqvw64e_device_handle, intel_driver::ntoskrnlAddr, current_function_data.name );
					if ( !function_address ) {
#if !defined(DISABLE_OUTPUT)
						std::cout << "[-] Failed to resolve import " << current_function_data.name << " (" << current_import.module_name << ")" << std::endl;
#endif
						return false;
					}
				}
			}

			*current_function_data.address = function_address;
		}
	}

	return true;
}