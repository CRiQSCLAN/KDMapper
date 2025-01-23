#include "kdmapper.hpp"

HANDLE iqvw64e_device_handle;

auto read_file(const std::string filename) -> std::vector<uint8_t>
{
	std::ifstream stream(filename, std::ios::binary);

	std::vector<uint8_t> buffer{ };

	buffer.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

	stream.close();

	return buffer;
}

int wmain(const int argc, wchar_t** argv) {
	SetConsoleTitle( "MAPPER" );
	
	MessageBoxA( 0, "are you sure you want to map?", 0, 0 );

	iqvw64e_device_handle = intel_driver::Load();

	if ( iqvw64e_device_handle == INVALID_HANDLE_VALUE )
	{
		printf( "[-] iqvw64e_device_handle failed to open handle.\n" );
		return -1;
	}

	printf( "[<] iqvw64e_device_handle handle created.\n" );

	std::vector<uint8_t> raw_image = read_file("driver.sys");

	NTSTATUS exitCode = 0;
	if (!kdmapper::MapDriver(iqvw64e_device_handle, raw_image.data(), 0, 0, false, true, true, true, NULL, &exitCode)) {
		printf("[-] Failed to map kernel device\n");
		intel_driver::Unload(iqvw64e_device_handle);
		return -1;
	}

	if (!intel_driver::Unload(iqvw64e_device_handle)) {
		printf("[-] Warning failed to fully unload vulnerable driver \n");
	}

	std::getchar();
}