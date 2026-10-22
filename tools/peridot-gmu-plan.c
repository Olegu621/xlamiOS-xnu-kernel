/* Host-only GMU image inspection using UniXNU's existing portable parser.
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror tools/peridot-gmu-plan.c
 *        vendor/AdrenoFirmware/AdrenoFirmware.c -o /tmp/peridot-gmu-plan
 * Usage: /tmp/peridot-gmu-plan gen71100_gmu.bin > plan.json
 * No firmware upload/execution; success proves format/bounds only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "../vendor/AdrenoFirmware/AdrenoFirmware.h"

/* gen7_gmu_vma[] in the manufacturer's SM8635 KGSL; see PERIDOT-GPU.md. */
static const AdrenoFirmwareRegion regions[] = {
    {0x00000000, 0x4000, 0}, {0x00004000, 0xffc000, 1},
    {0x10004000, 0x4000, 2}, {0x60000000, 0x20000000, 3},
    {0xc0000000, 0x20000000, 4}
};

static int inspect(const unsigned char *data, size_t size)
{
    AdrenoFirmwareBlock blocks[256];
    size_t count = 0;
    AdrenoFirmwareResult r = AdrenoFirmwarePlanGmu(data, size, regions,
        sizeof(regions) / sizeof(regions[0]), blocks, 256, &count);
    if (r != AdrenoFirmwareSuccess) {
        fprintf(stderr, "GMU image refused: parser result %d\n", r);
        return 1;
    }
    unsigned payloads = 0, preallocs = 0;
    int payload_seen = 0;
    for (size_t i = 0; i < count; i++) {
        const AdrenoFirmwareBlock *b = &blocks[i];
        if (b->bytes) {
            payloads++; payload_seen = 1;
        } else if (b->type == 1 || b->type == 7) {
            int valid = 0;
            for (size_t j = 0; j < sizeof(regions) / sizeof(regions[0]); j++) {
                const AdrenoFirmwareRegion *v = &regions[j];
                if (b->value && !(b->address & 3) && !(b->value & 3) &&
                    b->address >= v->address &&
                    (uint64_t)b->address + b->value <= (uint64_t)v->address + v->bytes)
                    valid = 1;
            }
            if (!valid || payload_seen) {
                fprintf(stderr, "Invalid/misordered GMU preallocation at block %zu\n", i);
                return 1;
            }
            preallocs++;
        }
    }
    if (!payloads) {
        fputs("GMU image contains no loadable payload\n", stderr);
        return 1;
    }
    printf("{\"bytes\":%zu,\"payloads\":%u,\"preallocations\":%u,\"blocks\":[\n",
        size, payloads, preallocs);
    for (size_t i = 0; i < count; i++) {
        const AdrenoFirmwareBlock *b = &blocks[i];
        printf("%s{\"address\":\"0x%08" PRIx32 "\",\"bytes\":%" PRIu32
            ",\"type\":%" PRIu32 ",\"value\":%" PRIu32 ",\"payload_offset\":%zu}",
            i ? ",\n" : "", b->address, b->bytes, b->type, b->value, b->payload_offset);
    }
    puts("\n]}");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fputs("Usage: peridot-gmu-plan gen71100_gmu.bin\n", stderr); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 2; }
    long length = ftell(f);
    if (length < 16 || length > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fputs("Invalid GMU image length (16B..4MiB required)\n", stderr);
        fclose(f); return 1;
    }
    unsigned char *data = malloc((size_t)length);
    if (!data) { fclose(f); return 2; }
    int result = 2;
    if (fread(data, 1, (size_t)length, f) == (size_t)length)
        result = inspect(data, (size_t)length);
    else fputs("Could not read complete GMU image\n", stderr);
    free(data); fclose(f);
    return result;
}
