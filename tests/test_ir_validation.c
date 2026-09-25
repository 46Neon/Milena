#include "typed_ir.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool append(MilenaIRProgram *program, uint32_t block, MilenaIROpCode opcode,
                   uint32_t result, MilenaIRType result_type, uint32_t left,
                   uint32_t right, int64_t integer, uint32_t yes,
                   uint32_t no) {
    return milena_ir_block_append_instruction(program, block, opcode, result, result_type,
                                       left, right, integer, 0.0, yes, no);
}

static void test_typed_values_and_return(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 40, 0, 0));
    assert(append(program, 1, MILENA_IR_CONST_I64, 2, MILENA_IR_TYPE_I64, 0, 0, 2, 0, 0));
    assert(append(program, 1, MILENA_IR_ADD_I64, 3, MILENA_IR_TYPE_I64, 1, 2, 0, 0, 0));
    assert(append(program, 1, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_I64, 3, 0, 0, 0, 0));
    assert(milena_ir_program_validate(program, error, sizeof(error)));
    assert(error[0] == '\0');
    milena_ir_program_destroy(program);
}

static void test_control_flow_targets(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 10));
    assert(append(program, 10, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 8, 0, 0));
    assert(append(program, 10, MILENA_IR_CONST_I64, 2, MILENA_IR_TYPE_I64, 0, 0, 8, 0, 0));
    assert(append(program, 10, MILENA_IR_EQ_I64, 3, MILENA_IR_TYPE_BOOL, 1, 2, 0, 0, 0));
    assert(append(program, 10, MILENA_IR_COND_BRANCH, 0, MILENA_IR_TYPE_VOID, 3, 0, 0, 20, 30));
    assert(milena_ir_program_add_block(program, 20));
    assert(append(program, 20, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(milena_ir_program_add_block(program, 30));
    assert(append(program, 30, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(milena_ir_program_validate(program, error, sizeof(error)));
    milena_ir_program_destroy(program);
}

static void test_rejects_type_mismatch(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 2, 0, 0));
    assert(append(program, 1, MILENA_IR_CONST_F64, 2, MILENA_IR_TYPE_F64, 0, 0, 0, 0, 0));
    assert(append(program, 1, MILENA_IR_ADD_I64, 3, MILENA_IR_TYPE_I64, 1, 2, 0, 0, 0));
    assert(append(program, 1, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "wrong type") != NULL);
    milena_ir_program_destroy(program);
}

static void test_rejects_undefined_and_duplicate_values(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_ADD_I64, 1, MILENA_IR_TYPE_I64, 77, 77, 0, 0, 0));
    assert(append(program, 1, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "undefined") != NULL);
    milena_ir_program_destroy(program);

    program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 4, MILENA_IR_TYPE_I64, 0, 0, 1, 0, 0));
    assert(append(program, 1, MILENA_IR_CONST_I64, 4, MILENA_IR_TYPE_I64, 0, 0, 2, 0, 0));
    assert(append(program, 1, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "duplicate") != NULL);
    milena_ir_program_destroy(program);
}

static void test_rejects_missing_terminator_and_unknown_opcode(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 7, 0, 0));
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    milena_ir_program_destroy(program);

    program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 7, 0, 0));
    assert(append(program, 1, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    program->instructions[0].opcode = MILENA_IR_OPCODE_COUNT;
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "unsupported") != NULL);
    milena_ir_program_destroy(program);
}

static void build_phi_diamond(MilenaIRProgram *program, bool wrong_edge_value) {
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 5, 0, 0));
    assert(append(program, 1, MILENA_IR_CONST_I64, 2, MILENA_IR_TYPE_I64, 0, 0, 5, 0, 0));
    assert(append(program, 1, MILENA_IR_EQ_I64, 3, MILENA_IR_TYPE_BOOL, 1, 2, 0, 0, 0));
    assert(append(program, 1, MILENA_IR_COND_BRANCH, 0, MILENA_IR_TYPE_VOID, 3, 0, 0, 10, 20));

    assert(milena_ir_program_add_block(program, 10));
    assert(append(program, 10, MILENA_IR_CONST_I64, 4, MILENA_IR_TYPE_I64, 0, 0, 40, 0, 0));
    assert(append(program, 10, MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 30, 0));

    assert(milena_ir_program_add_block(program, 20));
    if (wrong_edge_value)
        assert(milena_ir_block_append_instruction(program, 20, MILENA_IR_CONST_F64, 5,
                                           MILENA_IR_TYPE_F64, 0, 0, 0, 1.0, 0, 0));
    else
        assert(append(program, 20, MILENA_IR_CONST_I64, 5, MILENA_IR_TYPE_I64, 0, 0, 2, 0, 0));
    assert(append(program, 20, MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 30, 0));

    assert(milena_ir_program_add_block(program, 30));
    assert(milena_ir_program_add_block_parameter(program, 30, 6, MILENA_IR_TYPE_I64));
    assert(append(program, 30, MILENA_IR_ADD_I64, 7, MILENA_IR_TYPE_I64, 6, 1, 0, 0, 0));
    assert(append(program, 30, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_I64, 7, 0, 0, 0, 0));
    assert(milena_ir_block_add_edge_argument(program, 10, 30, 0, 4));
    assert(milena_ir_block_add_edge_argument(program, 20, 30, 0, 5));
}

static void test_dominance_and_block_parameter_value_flow(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    build_phi_diamond(program, false);
    assert(milena_ir_program_validate(program, error, sizeof(error)));
    milena_ir_program_destroy(program);

    /* A sibling-block definition cannot be smuggled across the other incoming
       edge: edge values must dominate their predecessor terminator. */
    program = milena_ir_program_create();
    assert(program != NULL);
    build_phi_diamond(program, false);
    program->edge_arguments[1].value_id = 4;
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "does not dominate") != NULL);
    milena_ir_program_destroy(program);

    program = milena_ir_program_create();
    assert(program != NULL);
    build_phi_diamond(program, true);
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "wrong type") != NULL);
    milena_ir_program_destroy(program);
}

static void test_loop_backedge_value_flow(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_CONST_I64, 1, MILENA_IR_TYPE_I64, 0, 0, 0, 0, 0));
    assert(append(program, 1, MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 10, 0));

    assert(milena_ir_program_add_block(program, 10));
    assert(milena_ir_program_add_block_parameter(program, 10, 2, MILENA_IR_TYPE_I64));
    assert(append(program, 10, MILENA_IR_CONST_I64, 3, MILENA_IR_TYPE_I64, 0, 0, 8, 0, 0));
    assert(append(program, 10, MILENA_IR_EQ_I64, 4, MILENA_IR_TYPE_BOOL, 2, 3, 0, 0, 0));
    assert(append(program, 10, MILENA_IR_COND_BRANCH, 0, MILENA_IR_TYPE_VOID, 4, 0, 0, 20, 30));

    assert(milena_ir_program_add_block(program, 20));
    assert(append(program, 20, MILENA_IR_CONST_I64, 5, MILENA_IR_TYPE_I64, 0, 0, 1, 0, 0));
    assert(append(program, 20, MILENA_IR_ADD_I64, 6, MILENA_IR_TYPE_I64, 2, 5, 0, 0, 0));
    assert(append(program, 20, MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 10, 0));

    assert(milena_ir_program_add_block(program, 30));
    assert(append(program, 30, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_I64, 2, 0, 0, 0, 0));
    assert(milena_ir_block_add_edge_argument(program, 1, 10, 0, 1));
    assert(milena_ir_block_add_edge_argument(program, 20, 10, 0, 6));
    assert(milena_ir_program_validate(program, error, sizeof(error)));
    milena_ir_program_destroy(program);
}

static void test_rejects_missing_or_duplicate_edge_arguments(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    build_phi_diamond(program, false);
    --program->edge_argument_count;
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "exactly once") != NULL);
    milena_ir_program_destroy(program);

    program = milena_ir_program_create();
    assert(program != NULL);
    build_phi_diamond(program, false);
    assert(milena_ir_block_add_edge_argument(program, 10, 30, 0, 4));
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "duplicate") != NULL);
    milena_ir_program_destroy(program);
}

static void test_rejects_undominated_direct_use_and_unreachable_block(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    build_phi_diamond(program, false);
    /* Replace the join's incoming block parameter with a use of a value that
       exists only in the left arm. The use itself is not dominated by it. */
    program->instructions[8].operand1_id = 4;
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "does not dominate") != NULL);
    milena_ir_program_destroy(program);

    program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(milena_ir_program_add_block(program, 2));
    assert(append(program, 2, MILENA_IR_RETURN, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 0, 0));
    assert(!milena_ir_program_validate(program, error, sizeof(error)));
    assert(strstr(error, "unreachable") != NULL);
    milena_ir_program_destroy(program);
}

static void test_rejects_missing_branch_target(void) {
    char error[160];
    MilenaIRProgram *program = milena_ir_program_create();
    assert(program != NULL);
    assert(milena_ir_program_add_block(program, 1));
    assert(append(program, 1, MILENA_IR_BRANCH, 0, MILENA_IR_TYPE_VOID, 0, 0, 0, 99, 0));
    assert(milena_ir_program_validate(program, error, sizeof(error)) == false);
    assert(strstr(error, "missing block") != NULL);
    milena_ir_program_destroy(program);
}

int main(void) {
    test_typed_values_and_return();
    test_control_flow_targets();
    test_rejects_type_mismatch();
    test_rejects_undefined_and_duplicate_values();
    test_rejects_missing_terminator_and_unknown_legacy_opcode();
    test_rejects_missing_branch_target();
    test_dominance_and_block_parameter_value_flow();
    test_loop_backedge_value_flow();
    test_rejects_missing_or_duplicate_edge_arguments();
    test_rejects_undominated_direct_use_and_unreachable_block();
    puts("typed IR structural/type/control-flow validation tests passed");
    return 0;
}
