#include <stdio.h>
#include <stdint.h>
#include "tools/apk_builder/zip_writer.h"
int main(void) {
    const unsigned char txt[] = "hello zip";
    const unsigned char blob[] = {0x7f,'E','L','F',0,1,2,3};
    FILE* f = fopen("debug_hello.txt", "wb"); fwrite(txt,1,sizeof(txt)-1,f); fclose(f);
    f = fopen("debug_blob.bin", "wb"); fwrite(blob,1,sizeof(blob),f); fclose(f);
    ZipWriter* zw = zip_writer_open("debug_roundtrip.apk");
    if (!zw) return 2;
    if (zip_writer_add_file(zw, "assets/hello.txt", "debug_hello.txt", 0) != 0) return 3;
    if (zip_writer_add_file(zw, "lib/arm64-v8a/libMainActivity.so", "debug_blob.bin", 0) != 0) return 4;
    if (zip_writer_close(zw) != 0) return 5;
    CpxZipError err = cpx_zip_verify("debug_roundtrip.apk");
    printf("verify=%d %s\n", (int)err, cpx_zip_error_string(err));
    return (int)err;
}
