#include "compiler/ir/mir_lower.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void run_case(DataType return_type, DataType parameter_type) {
    Stmt function;
    Parameter parameter;
    Stmt* statements[1];
    MirModule* module;

    memset(&function, 0, sizeof(function));
    memset(&parameter, 0, sizeof(parameter));
    function.type = STMT_FUNCTION;
    function.as.function.name = "lowering_width_probe";
    function.as.function.return_type = return_type;
    function.as.function.parameters = &parameter;
    function.as.function.param_count = 1;
    parameter.name = "wide_value";
    parameter.type = parameter_type;
    statements[0] = &function;

    /* This deliberately bypasses semantic validation and tests the defensive
     * MIR-lowering boundary directly. A failed lowering must return no module. */
    module = mir_lower_program(statements, 1, NULL, "unsupported_width_probe");
    if (module) {
        fprintf(stderr, "unsupported type escaped MIR lowering: return=%d parameter=%d\n",
                (int)return_type, (int)parameter_type);
        mir_module_destroy(module);
        failures++;
    }
}

int main(void) {
    run_case(TYPE_I128, TYPE_I64);
    run_case(TYPE_I64, TYPE_U128);
    if (failures) return 1;
    puts("direct MIR unsupported-width lowering rejection: 2 passed");
    return 0;
}
