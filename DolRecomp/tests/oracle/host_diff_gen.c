#include <stdio.h>
#include <stdlib.h>

#include "backend/emitter.h"
#include "frontend/decoder.h"
#include "host_diff.h"
#include "host_fixture.inc"

int main(int argc, char** argv) {
    FILE* out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) {
        perror("open generated C");
        return 2;
    }

    emit_header(out);
    for (unsigned i = 0; i < host_oracle_case_count; i++) {
        const HostOracleCase* c = &host_oracle_cases[i];
        PPCInst insts[HOST_ORACLE_MAX_WORDS];
        for (u32 j = 0; j < c->raw_count; j++) {
            insts[j] = ppc_decode(c->raw[j], c->address + j * 4u);
            if (insts[j].op == PPC_OP_UNKNOWN) {
                fprintf(stderr, "%s[%u] decoded as unknown\n", c->name, j);
                return 2;
            }
        }
        emit_function(out, insts, c->raw_count, c->address);
    }
    fprintf(out, "void (*const host_oracle_funcs[])(CPUState*) = {\n");
    for (unsigned i = 0; i < host_oracle_case_count; i++)
        fprintf(out, "    func_%08X,\n", host_oracle_cases[i].address);
    fprintf(out, "};\n");
    emit_footer(out);

    if (out != stdout)
        fclose(out);
    return 0;
}
