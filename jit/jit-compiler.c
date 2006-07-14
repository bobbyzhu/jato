/*
 * Copyright (C) 2005-2006  Pekka Enberg
 *
 * This file is released under the GPL version 2. Please refer to the file
 * LICENSE for details.
 */

#include <vm/alloc.h>
#include <jit/compilation-unit.h>
#include <errno.h>
#include <jit-compiler.h>
#include <jit/statement.h>
#include <insn-selector.h>
#include <x86-objcode.h>
#include <vm/string.h>
#include <jit/tree-printer.h>

#include "disass.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define OBJCODE_SIZE 256

int show_disasm;
int show_tree;

static void print_method_info(struct methodblock *method)
{
	printf("Method: %s, Class: %s\n", method->name, CLASS_CB(method->class)->name);
}

static void print_disasm(struct methodblock *method, void *start, void *end)
{
	print_method_info(method);
	disassemble(start, end);
	printf("\n");
}

static void print_tree(struct compilation_unit *cu)
{
	struct basic_block *bb;
	struct statement *stmt;
	struct string *str;
	
	print_method_info(cu->method);

	str = alloc_str();

	list_for_each_entry(bb, &cu->bb_list, bb_list_node) {
		list_for_each_entry(stmt, &bb->stmt_list, stmt_list_node)
			tree_print(&stmt->node, str);
	}

	printf(str->value);
	printf("\n");
	free_str(str);
}

int jit_compile(struct compilation_unit *cu)
{
	int err = 0;
	struct insn_sequence is;

	err = build_cfg(cu);
	if (err)
		goto out;

	err = convert_to_ir(cu);
	if (err)
		goto out;

	if (show_tree)
		print_tree(cu);

	insn_select(bb_entry(cu->bb_list.next));

	cu->objcode = alloc_exec(OBJCODE_SIZE);
	if (!cu->objcode) {
		err = -ENOMEM;
		goto out;
	}
	memset(cu->objcode, 0, OBJCODE_SIZE);
	init_insn_sequence(&is, cu->objcode, OBJCODE_SIZE);
	x86_emit_prolog(&is, 0);
	x86_emit_obj_code(bb_entry(cu->bb_list.next), &is);
	x86_emit_epilog(&is, 0);

	if (show_disasm)
		print_disasm(cu->method, is.start, is.current);

	cu->is_compiled = true;

  out:
	if (err) {
		struct classblock *cb = CLASS_CB(cu->method->class);
		printf("%s: Failed to compile method `%s' in class `%s', error: %i\n",
		       __FUNCTION__, cu->method->name, cb->name, err);
	}

	return err;
}

void *jit_magic_trampoline(struct compilation_unit *cu)
{
	pthread_mutex_lock(&cu->mutex);

	if (!cu->is_compiled)
		jit_compile(cu);

	pthread_mutex_unlock(&cu->mutex);

	return cu->objcode;
}

#define TRAMP_OBJSIZE 15

static struct jit_trampoline *alloc_jit_trampoline(void)
{
	struct jit_trampoline *tramp = malloc(sizeof(*tramp));
	if (!tramp)
		return NULL;

	memset(tramp, 0, sizeof(*tramp));

	tramp->objcode = alloc_exec(TRAMP_OBJSIZE);
	if (!tramp->objcode)
		goto failed;

	memset(tramp->objcode, 0, TRAMP_OBJSIZE);

	return tramp;

  failed:
	free_jit_trampoline(tramp);
	return NULL;
}

void free_jit_trampoline(struct jit_trampoline *tramp)
{
	free(tramp->objcode);
	free(tramp);
}

struct jit_trampoline *build_jit_trampoline(struct compilation_unit *cu)
{
	struct jit_trampoline *tramp = alloc_jit_trampoline();
	if (tramp)
		x86_emit_trampoline(cu, jit_magic_trampoline, tramp->objcode,
				    TRAMP_OBJSIZE);
	return tramp;
}

void *jit_prepare_for_exec(struct methodblock *mb)
{
	mb->compilation_unit = alloc_compilation_unit(mb);
	mb->trampoline = build_jit_trampoline(mb->compilation_unit);

	return mb->trampoline->objcode;
}
