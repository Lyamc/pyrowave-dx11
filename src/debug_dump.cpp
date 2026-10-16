// debug_dump.cpp -- Debug dump infrastructure implementation (T6c)
// Writes manifest.json + *.bin files in VALIDATION §3.4 format.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "debug_dump.h"

// ------------------------------------------------------------------
// SHA-256 via Windows CryptoAPI
// ------------------------------------------------------------------
static int sha256_hex(const void *data, size_t len, char *out_hex, size_t out_hex_len)
{
    HCRYPTPROV hprov = 0;
    HCRYPTHASH hhash = 0;
    unsigned char hash[32];
    DWORD hash_len = 32;
    int result = -1;

    if (!CryptAcquireContext(&hprov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        goto cleanup;
    if (!CryptCreateHash(hprov, CALG_SHA_256, 0, 0, &hhash))
        goto cleanup;
    if (!CryptHashData(hhash, (const unsigned char *)data, (DWORD)len, 0))
        goto cleanup;
    if (!CryptGetHashParam(hhash, HP_HASHVAL, hash, &hash_len, 0))
        goto cleanup;

    for (DWORD i = 0; i < hash_len; i++) {
        if (i * 2 + 3 > out_hex_len) { result = -1; goto cleanup; }
        sprintf_s(out_hex + i * 2, 3, "%02x", hash[i]);
    }
    out_hex[hash_len * 2] = '\0';
    result = 0;

cleanup:
    if (hhash) CryptDestroyHash(hhash);
    if (hprov) CryptReleaseContext(hprov, 0);
    return result;
}

// ------------------------------------------------------------------
// Directory creation (recursive)
// ------------------------------------------------------------------
static void mkdirs(const char *path)
{
    char buf[MAX_PATH];
    strncpy_s(buf, sizeof(buf), path, _TRUNCATE);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            CreateDirectoryA(buf, NULL);
            *p = '/';
        }
    }
    CreateDirectoryA(buf, NULL);
}

// ------------------------------------------------------------------
// Dump a single buffer
// ------------------------------------------------------------------
int debug_dump_buffer(const char *dump_dir, const char *name, int stage, int dtype,
                      int shape_w, int shape_h, const void *data, size_t data_len)
{
    char path[MAX_PATH];
    char sha_hex[65];

    // Create directory
    mkdirs(dump_dir);

    // Write buffer file
    snprintf(path, sizeof(path), "%s/%s.bin", dump_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "debug_dump: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fwrite(data, 1, data_len, f);
    fclose(f);

    // Compute SHA-256
    if (sha256_hex(data, data_len, sha_hex, sizeof(sha_hex)) != 0) {
        fprintf(stderr, "debug_dump: SHA-256 failed for %s\n", name);
        return -1;
    }

    // Store in global registry (simple static buffer for now)
    // In a full implementation, this would be per-encoder state
    // For now, we just write the manifest at the end
    (void)stage; (void)dtype; (void)shape_w; (void)shape_h;

    return 0;
}

// ------------------------------------------------------------------
// Write manifest.json
// ------------------------------------------------------------------
int debug_write_manifest(const char *dump_dir, const char *vector_name, int w, int h,
                         int budget, const char *device_name,
                         const char **buffer_names, int *stages, int *dtypes,
                         int *shapes_w, int *shapes_h, int count)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/manifest.json", dump_dir);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "debug_dump: cannot write manifest %s: %s\n", path, strerror(errno));
        return -1;
    }

    // Compute SHA-256 for each buffer
    char sha_hexes[128][65];  // max 128 buffers

    fprintf(f, "{\n");
    fprintf(f, "  \"vector\": \"%s\",\n", vector_name);
    fprintf(f, "  \"W\": %d,\n", w);
    fprintf(f, "  \"H\": %d,\n", h);
    fprintf(f, "  \"budget\": %d,\n", budget);
    fprintf(f, "  \"device\": \"%s\",\n", device_name);
    fprintf(f, "  \"ref\": \"pyrowave-d3d11-debug\",\n");
    fprintf(f, "  \"buffers\": [\n");

    for (int i = 0; i < count; i++) {
        char buf_path[MAX_PATH];
        snprintf(buf_path, sizeof(buf_path), "%s/%s.bin", dump_dir, buffer_names[i]);

        // Read file and compute SHA-256
        FILE *bf = fopen(buf_path, "rb");
        if (!bf) {
            fprintf(stderr, "debug_dump: cannot read %s for manifest\n", buf_path);
            sha_hexes[i][0] = '\0';
            continue;
        }
        fseek(bf, 0, SEEK_END);
        long flen = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        void *buf = malloc(flen);
        fread(buf, 1, flen, bf);
        fclose(bf);

        sha256_hex(buf, flen, sha_hexes[i], sizeof(sha_hexes[i]));
        free(buf);

        const char *dtype_str = dtypes[i] == 0 ? "f32" : "f16";
        fprintf(f, "    {\"name\": \"%s\", \"stage\": %d, \"dtype\": \"%s\", "
                       "\"shape\": [%d, %d], \"file\": \"%s.bin\", \"sha256\": \"%s\"}",
                buffer_names[i], stages[i], dtype_str, shapes_w[i], shapes_h[i],
                buffer_names[i], sha_hexes[i]);
        if (i < count - 1) fprintf(f, ",");
        fprintf(f, "\n");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}
