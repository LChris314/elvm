#include <ir/ir.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ir/table.h>

static bool g_split_basic_block_by_mem = false;
static bool g_handle_call_ret = false;
static bool g_handle_logic_ops = false;

typedef struct DataPrivate_ {
  int v;
  struct DataPrivate_* next;
  Value val;
  int lineno;
} DataPrivate;

typedef struct {
  const char* filename;
  int lineno;
  int col;
  FILE* fp;
  Table* symtab;
  int in_text;
  Inst* text;
  int pc;
  int subsection;
  DataPrivate* data;
  const char* func;
  bool prev_boundary;
} Parser;

enum {
  DATA = LAST_OP + 1, TEXT, LONG, COMM, ZERO, STRING,
  FILENAME, LOC, SECTION, SKIP
};

enum {
  REF = IMM + 1, LABEL
};

#ifdef __GNUC__
__attribute__((noreturn))
#endif
static void ir_error(Parser* p, const char* msg) {
  fprintf(stderr, "%s:%d:%d: %s\n", p->filename, p->lineno, p->col, msg);
  exit(1);
}

static int ir_getc(Parser* p) {
  int c = fgetc(p->fp);
  if (c == '\n') {
    p->lineno++;
    p->col = 0;
  } else {
    p->col++;
  }
  return c;
}

static void ir_ungetc(Parser* p, int c) {
  if (c == '\n') {
    p->lineno--;
  }
  ungetc(c, p->fp);
}

static int peek(Parser* p) {
  int c = fgetc(p->fp);
  ungetc(c, p->fp);
  return c;
}

static void skip_until_ret(Parser* p) {
  int c;
  for (;;) {
    c = ir_getc(p);
    if (c == '\n' || c == EOF)
      break;
  }
  ir_ungetc(p, c);
}

static void skip_ws(Parser* p) {
  int c;
  for (;;) {
    c = ir_getc(p);
    if (!isspace(c))
      break;
  }
  ir_ungetc(p, c);
}

static void read_while_ident(Parser* p, char* buf, int len) {
  while (len--) {
    int c = ir_getc(p);
    if (!isalnum(c) && c != '_' && c != '.') {
      ir_ungetc(p, c);
      *buf = 0;
      return;
    }
    *buf++ = c;
  }
  ir_error(p, "too long ident");
}

static int read_int(Parser* p, int c) {
  bool is_minus = false;
  int r = 0;
  if (c == '-') {
    is_minus = true;
    c = ir_getc(p);
    if (!isdigit(c))
      ir_error(p, "digit expected");
  }
  while ('0' <= c && c <= '9') {
    r *= 10;
    r += c - '0';
    c = ir_getc(p);
  }
  ir_ungetc(p, c);
  return is_minus ? -r : r;
}

static DataPrivate* add_data(Parser* p) {
  DataPrivate* n = malloc(sizeof(DataPrivate));
  n->next = 0;
  n->v = p->subsection;
  n->lineno = p->lineno;
  p->data->next = n;
  p->data = n;
  return n;
}

static void add_imm_data(Parser* p, int v) {
  DataPrivate* n = add_data(p);
  n->val.type = IMM;
  n->val.imm = v;
}

static void serialize_data(Parser* p, DataPrivate* data_root) {
  bool done = false;
  DataPrivate serialized_root = {};
  DataPrivate* serialized = &serialized_root;
  intptr_t mp = 0;
  for (int subsection = 0; !done; subsection++) {
    done = true;
    for (DataPrivate* data = data_root; data->next;) {
      DataPrivate* prev = data;
      data = prev->next;
      if (data->v != subsection) {
        continue;
      }
      done = false;

      prev->next = data->next;

      if (data->val.type == (ValueType)LABEL) {
        p->symtab = table_add(p->symtab, data->val.tmp, (void*)mp);
      } else {
        serialized->next = data;
        serialized = data;
        serialized->next = 0;
        mp++;
      }
      data = prev;
    }
  }

  p->symtab = table_add(p->symtab, "_edata", (void*)mp);
  serialized->next = malloc(sizeof(DataPrivate));
  serialized->next->v = mp + 1;
  serialized->next->next = 0;
  serialized->next->val.type = IMM;
  serialized->next->val.imm = mp + 1;
  data_root->next = serialized_root.next;
}

static Op get_op(Parser* p, const char* buf) {
  if (peek(p) == ':')
    return OP_UNSET;
  if (!strcmp(buf, "mov")) {
    return MOV;
  } else if (!strcmp(buf, "add")) {
    return ADD;
  } else if (!strcmp(buf, "sub")) {
    return SUB;
  } else if (!strcmp(buf, "load")) {
    return LOAD;
  } else if (!strcmp(buf, "store")) {
    return STORE;
  } else if (!strcmp(buf, "putc")) {
    return PUTC;
  } else if (!strcmp(buf, "getc")) {
    return GETC;
  } else if (!strcmp(buf, "exit")) {
    return EXIT;
  } else if (!strcmp(buf, "dump")) {
    return DUMP;
  } else if (!strcmp(buf, "jeq")) {
    return JEQ;
  } else if (!strcmp(buf, "jne")) {
    return JNE;
  } else if (!strcmp(buf, "jlt")) {
    return JLT;
  } else if (!strcmp(buf, "jgt")) {
    return JGT;
  } else if (!strcmp(buf, "jle")) {
    return JLE;
  } else if (!strcmp(buf, "jge")) {
    return JGE;
  } else if (!strcmp(buf, "jmp")) {
    return JMP;
  } else if (!strcmp(buf, "eq")) {
    return EQ;
  } else if (!strcmp(buf, "ne")) {
    return NE;
  } else if (!strcmp(buf, "lt")) {
    return LT;
  } else if (!strcmp(buf, "gt")) {
    return GT;
  } else if (!strcmp(buf, "le")) {
    return LE;
  } else if (!strcmp(buf, "ge")) {
    return GE;
  } else if (!strcmp(buf, "call")) {
    return CALL;
  } else if (!strcmp(buf, "ret")) {
    return RET;
  } else if (!strcmp(buf, "and")) {
    return AND;
  } else if (!strcmp(buf, "or")) {
    return OR;
  } else if (!strcmp(buf, "xor")) {
    return XOR;
  } else if (!strcmp(buf, "sll")) {
    return SLL;
  } else if (!strcmp(buf, "srl")) {
    return SRL;
  } else if (!strcmp(buf, ".text")) {
    return TEXT;
  } else if (!strcmp(buf, ".data")) {
    return DATA;
  } else if (!strcmp(buf, ".long") ||
             !strcmp(buf, ".byte")) {
    return LONG;
  } else if (!strcmp(buf, ".comm")) {
    return COMM;
  } else if (!strcmp(buf, ".zero")) {
    return ZERO;
  } else if (!strcmp(buf, ".string") ||
             !strcmp(buf, ".asciz") ||
             !strcmp(buf, ".ascii")) {
    return STRING;
  } else if (!strcmp(buf, ".file")) {
    return FILENAME;
  } else if (!strcmp(buf, ".loc")) {
    return LOC;
  } else if (!strcmp(buf, ".section")) {
    return SECTION;
  } else if (!strcmp(buf, ".globl") ||
             !strcmp(buf, ".local") ||
             !strcmp(buf, ".cfi_startproc") ||
             !strcmp(buf, ".cfi_endproc")) {
    // TODO: Probably we can remove this in LLVM
    return SKIP;
  }
  return OP_UNSET;
}

static void alloc_inst(Parser* p, Op op) {
  p->text->next = calloc(1, sizeof(Inst));
  p->text = p->text->next;
  p->text->op = op;
  p->text->pc = p->pc;
  p->text->lineno = p->lineno;
  p->text->func = p->func;
}

static void emit_inst_ri(Parser* p, Op op, Reg r, int i) {
  alloc_inst(p, op);
  p->text->dst = (Value){ .type = REG, .reg = r };
  p->text->src = (Value){ .type = IMM, .imm = i };
}

static void emit_inst_rr(Parser* p, Op op, Reg r, Reg r2) {
  alloc_inst(p, op);
  p->text->dst = (Value){ .type = REG, .reg = r };
  p->text->src = (Value){ .type = REG, .reg = r2 };
}

static void emit_push(Parser* p, Reg r) {
  emit_inst_ri(p, SUB, SP, 1);
  emit_inst_rr(p, STORE, r, SP);
}

static void emit_pop(Parser* p, Reg r) {
  emit_inst_rr(p, LOAD, r, SP);
  emit_inst_ri(p, ADD, SP, 1);
}

static void emit_call(Parser* p, const char* fname) {
  emit_inst_ri(p, SUB, SP, 1);
  emit_inst_ri(p, MOV, D, p->pc + 1);
  emit_inst_rr(p, STORE, D, SP);
  alloc_inst(p, JMP);
  p->text->jmp = (Value){ .type = REF, .tmp = (void*)fname };
  p->pc++;
}

static void parse_line(Parser* p, int c) {
  char buf[64];
  buf[0] = c;
  read_while_ident(p, buf + 1, 62);
  Op op = get_op(p, buf);
  if (op == (Op)TEXT) {
    p->in_text = 1;
    return;
  } else if (op == (Op)STRING) {
    if (p->in_text)
      ir_error(p, "in text");
    skip_ws(p);
    if (ir_getc(p) != '"')
      ir_error(p, "expected open '\"'");

    c = ir_getc(p);
    while (c != '"') {
      if (c == '\\') {
        c = ir_getc(p);
        if (c == 'n') {
          c = '\n';
        } else if (c == 't') {
          c = '\t';
        } else if (c == 'b') {
          c = '\b';
        } else if (c == 'f') {
          c = '\f';
        } else if (c == 'r') {
          c = '\r';
        } else if (c == '\"') {
          c = '\"';
        } else if (c == '\\') {
          c = '\\';
        } else if (c == 'x') {
          char b[3];
          b[0] = ir_getc(p);
          c = ir_getc(p);
          if (!((c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))) {
            ir_ungetc(p, c);
            c = 0;
          }
          b[1] = c;
          b[2] = 0;
          c = strtoul(b, NULL, 16);
        } else if (isdigit(c)) {
          char b[4];
          b[0] = c;
          b[1] = ir_getc(p);
          b[2] = ir_getc(p);
          b[3] = 0;
          c = strtoul(b, NULL, 8);
        } else {
          ir_error(p, "unknown escape");
        }
        add_imm_data(p, c);
      } else {
        add_imm_data(p, c);
      }
      c = ir_getc(p);
    }
    add_imm_data(p, 0);
    return;
  } else if (op == (Op)FILENAME) {
    skip_until_ret(p);
    return;
  } else if (op == (Op)LOC) {
    skip_until_ret(p);
    return;
  } else if (op == (Op)SECTION) {
    skip_ws(p);
    read_while_ident(p, buf, 63);
    // Stop parsing after finding a debug section.
    // TODO: Implement strncmp.
    buf[6] = 0;
    if (!strcmp(buf, ".debug")) {
      while (ir_getc(p) != EOF) {}
    } else if (!strcmp(buf, ".rodat")) {
      p->in_text = false;
      skip_until_ret(p);
    } else {
      skip_until_ret(p);
    }
    return;
  } else if (op == (Op)SKIP) {
    skip_until_ret(p);
    return;
  } else if (op == OP_UNSET) {
    c = ir_getc(p);
    if (c == ':') {
      intptr_t value = 0;
      if (p->in_text) {
        if (!p->prev_boundary)
          p->pc++;
        value = p->pc;
        p->prev_boundary = true;
        const char* func = strdup(buf);
        if (func[0] != 'L' && func[0] != '.')
          p->func = func;
        p->symtab = table_add(p->symtab, func, (void*)value);
      } else {
        DataPrivate* d = add_data(p);
        d->val.type = LABEL;
        d->val.tmp = strdup(buf);
      }
      return;
    }
    ir_error(p, "unknown op");
  }

  if (op < 0) {
    ir_error(p, "oops");
  }
  int argc;
  if (op <= STORE)
    argc = 2;
  else if (op <= GETC)
    argc = 1;
  else if (op <= EXIT)
    argc = 0;
  else if (op <= JGE)
    argc = 3;
  else if (op <= JMP)
    argc = 1;
  else if (op <= GE)
    argc = 2;
  else if (op == CALL)
    argc = 1;
  else if (op == RET)
    argc = 0;
  else if (op <= SRL)
    argc = 2;
  else if (op == DUMP)
    argc = 0;
  else if (op == (Op)LONG)
    argc = 1;
  else if (op == (Op)COMM)
    argc = 3;
  else if (op == (Op)ZERO)
    argc = 1;
  else if (op == (Op)DATA) {
    skip_ws(p);
    c = peek(p);
    argc = c == '-' || isdigit(c) ? 1 : 0;
  } else
    ir_error(p, "oops");

  Value args[3];
  for (int i = 0; i < argc; i++) {
    skip_ws(p);
    if (i) {
      c = ir_getc(p);
      if (c != ',')
        ir_error(p, "comma expected");
      skip_ws(p);
    }

    Value a;
    c = ir_getc(p);
    if (isdigit(c) || c == '-') {
      a.type = IMM;
      a.imm = MOD24(read_int(p, c));
    } else {
      buf[0] = c;
      read_while_ident(p, buf + 1, 62);
      a.type = REG;
      if (!strcmp(buf, "A")) {
        a.reg = A;
      } else if (!strcmp(buf, "B")) {
        a.reg = B;
      } else if (!strcmp(buf, "C")) {
        a.reg = C;
      } else if (!strcmp(buf, "D")) {
        a.reg = D;
      } else if (!strcmp(buf, "SP")) {
        a.reg = SP;
      } else if (!strcmp(buf, "BP")) {
        a.reg = BP;
      } else {
        a.type = (ValueType)REF;
        a.tmp = strdup(buf);
      }
    }
    args[i] = a;
  }

  if (op == (Op)LONG) {
    if (args[0].type == IMM) {
      add_imm_data(p, args[0].imm);
    } else if (args[0].type == (ValueType)REF) {
      DataPrivate* d = add_data(p);
      d->val.type = REF;
      d->val.tmp = args[0].tmp;
    } else {
      ir_error(p, "number expected");
    }
    return;
  } else if (op == (Op)COMM) {
    if (args[0].type != (ValueType)REF) {
      ir_error(p, "identifier expected");
    }
    DataPrivate* d = add_data(p);
    d->val.type = LABEL;
    d->val.tmp = strdup(args[0].tmp);

    if (args[1].type != IMM || args[2].type != IMM) {
      ir_error(p, "number expected");
    }
    int n = args[1].imm * args[2].imm;
    for (int i = 0; i < n; i++) {
      add_imm_data(p, 0);
    }
    return;
  } else if (op == (Op)ZERO) {
    if (args[0].type != IMM) {
      ir_error(p, "number expected");
    }
    int n = args[0].imm;
    for (int i = 0; i < n; i++) {
      add_imm_data(p, 0);
    }
    return;
  } else if (op == (Op)DATA) {
    if (argc == 1) {
      if (args[0].type != IMM)
        ir_error(p, "number expected");
      p->subsection = args[0].imm;
    }
    p->in_text = 0;
    return;
  }

  Inst* orig_text_head = p->text;
  alloc_inst(p, op);
  p->prev_boundary = false;
  switch (op) {
    case LOAD:
    case STORE:
      if (g_split_basic_block_by_mem) {
        p->pc++;
        p->prev_boundary = true;
      }
    case MOV:
    case ADD:
    case SUB:
    case EQ:
    case NE:
    case LT:
    case GT:
    case LE:
    case GE:
      p->text->src = args[1];
    case GETC:
      p->text->dst = args[0];
      break;
    case PUTC:
      p->text->src = args[0];
    case EXIT:
    case DUMP:
      break;
    case RET:
      if (!g_handle_call_ret) {
        p->text->op = LOAD;
        p->text->dst = (Value){ .type = REG, .reg = D };
        p->text->src = (Value){ .type = REG, .imm = SP };
        alloc_inst(p, ADD);
        p->text->dst = (Value){ .type = REG, .reg = SP };
        p->text->src = (Value){ .type = IMM, .imm = 1 };
        alloc_inst(p, JMP);
        p->text->jmp = (Value){ .type = REG, .reg = D };
      }
      p->pc++;
      p->prev_boundary = true;
      break;
    case JEQ:
    case JNE:
    case JLT:
    case JGT:
    case JLE:
    case JGE:
      p->text->dst = args[1];
      p->text->src = args[2];
    case JMP:
      p->text->jmp = args[0];
      p->pc++;
      p->prev_boundary = true;
      break;
    case CALL:
      if (!g_handle_call_ret) {
        p->text->op = SUB;
        p->text->dst = (Value){ .type = REG, .reg = SP };
        p->text->src = (Value){ .type = IMM, .imm = 1 };
        alloc_inst(p, MOV);
        p->text->dst = (Value){ .type = REG, .reg = D };
        p->text->src = (Value){ .type = IMM, .imm = p->pc + 1 };
        alloc_inst(p, STORE);
        p->text->dst = (Value){ .type = REG, .reg = D };
        p->text->src = (Value){ .type = REG, .reg = SP };
        alloc_inst(p, JMP);
      }
      p->text->jmp = args[0];
      p->pc++;
      p->prev_boundary = true;
      break;

  case AND:
  case OR:
  case XOR:
  case SLL:
  case SRL:
    if (g_handle_logic_ops) {
      p->text->src = args[1];
      p->text->dst = args[0];
    } else {
      p->text = orig_text_head;
      Reg reg = args[0].reg;
      Reg tmp = -1;
      for (int i = 0; i < 4; i++) {
        Reg r = (Reg)i;
        if (r != reg) {
          tmp = r;
          emit_push(p, r);
        }
      }

      // Push arguments.
      emit_inst_ri(p, MOV, tmp, 0);
      p->text->src = args[1];
      emit_inst_ri(p, SUB, SP, 1);
      emit_inst_rr(p, STORE, tmp, SP);
      emit_push(p, reg);

      const char* fname = NULL;
      if (op == AND)
        fname = "__elvm_builtin_and";
      else if (op == OR)
        fname = "__elvm_builtin_or";
      else if (op == XOR)
        fname = "__elvm_builtin_xor";
      else if (op == SLL)
        fname = "__elvm_builtin_shl";
      else if (op == SRL)
        fname = "__elvm_builtin_shr";
      else
        abort();
      emit_call(p, fname);

      emit_inst_ri(p, ADD, SP, 2);
      if (reg != A)
        emit_inst_rr(p, MOV, reg, A);
      for (int i = 4; i > 0;) {
        i--;
        Reg r = (Reg)i;
        if (r != reg)
          emit_pop(p, r);
      }
    }
    break;

  default:
      ir_error(p, "oops");
  }

#if 0
  dump_inst(p->text);
#endif
}

static void parse_eir(Parser* p) {
  Inst text_root = {};
  DataPrivate data_root = {};
  int c;

  p->in_text = 1;
  p->lineno = 1;
  p->text = &text_root;
  p->data = &data_root;
  p->pc = 0;
  p->prev_boundary = true;

  alloc_inst(p, JMP);
  p->pc++;
  p->text->lineno = -1;
  p->text->jmp.type = (ValueType)REF;
  p->text->jmp.tmp = "main";
  p->text->next = 0;
  p->symtab = table_add(p->symtab, "main", (void*)1);

  for (;;) {
    skip_ws(p);
    c = ir_getc(p);
    if (c == EOF)
      break;

    if (c == '#') {
      skip_until_ret(p);
    } else if (c == '_' || c == '.' || isalpha(c)) {
      parse_line(p, c);
    } else {
      ir_error(p, "unexpected char");
    }
  }

  serialize_data(p, &data_root);
  p->text = text_root.next;
  p->data = data_root.next;

  // For new ABI which uses _start as the entry point.
  const void* unused;
  if (table_get(p->symtab, "_start", &unused)) {
    p->text->jmp.tmp = "_start";
  }
}

static bool resolve(Value* v, Table* symtab) {
  if (v->type != (ValueType)REF)
    return true;
  const char* name = (const char*)v->tmp;
  if (!table_get(symtab, name, (void*)&v->imm)) {
    fprintf(stderr, "undefined sym: %s\n", name);
    return false;
  }
  //fprintf(stderr, "resolved: %s %d\n", name, v->imm);
  v->type = IMM;
  return true;
}

static void resolve_syms(Parser* p) {
  bool ok = true;
  for (DataPrivate* data = p->data; data; data = data->next) {
    if (data->val.type == (ValueType)REF) {
      ok &= resolve(&data->val, p->symtab);
    }
    data->v = MOD24(data->val.imm);
  }

  for (Inst* inst = p->text; inst; inst = inst->next) {
    ok &= resolve(&inst->dst, p->symtab);
    ok &= resolve(&inst->src, p->symtab);
    ok &= resolve(&inst->jmp, p->symtab);
  }
  if (!ok)
    exit(1);
}

Module* load_eir_impl(const char* filename, FILE* fp) {
  Parser parser = {
    .filename = filename,
    .fp = fp,
    .func = "???",
  };
  parse_eir(&parser);
  resolve_syms(&parser);

  Module* m = malloc(sizeof(Module));
  m->text = parser.text;
  m->data = (Data*)parser.data;
  return m;
}

Module* load_eir(FILE* fp) {
  return load_eir_impl("<stdin>", fp);
}

Module* load_eir_from_file(const char* filename) {
  FILE* fp = fopen(filename, "r");
  if (!fp) {
    fprintf(stderr, "no such file: %s\n", filename);
    exit(1);
  }
  Module* r = load_eir_impl(filename, fp);
  fclose(fp);
  return r;
}

void split_basic_block_by_mem() {
  g_split_basic_block_by_mem = true;
}

void handle_call_ret() {
  g_handle_call_ret = true;
}

void dump_op(Op op, FILE* fp) {
  static const char* op_strs[] = {
    "mov", "add", "sub", "load", "store", "putc", "getc", "exit",
    "jeq", "jne", "jlt", "jgt", "jle", "jge", "jmp", "xxx",
    "eq", "ne", "lt", "gt", "le", "ge",
    "call", "ret", "and", "or", "xor", "sll", "srl",
    "dump"
  };
  fprintf(fp, "%s", op_strs[op]);
}

void dump_val(Value* val, FILE* fp) {
  static const char* reg_strs[] = {
    "A", "B", "C", "D", "BP", "SP"
  };
  if (val->type == REG) {
    fprintf(fp, "%s", reg_strs[val->reg]);
  } else if (val->type == IMM) {
    fprintf(fp, "%d", val->imm);
  } else {
    fprintf(fp, "%d (type=%d)", val->imm, val->type);
  }
}

void dump_inst_fp(Inst* inst, FILE* fp) {
  dump_op(inst->op, fp);
  switch (inst->op) {
    case MOV:
    case ADD:
    case SUB:
    case LOAD:
    case STORE:
    case EQ:
    case NE:
    case LT:
    case GT:
    case LE:
    case GE:
      fprintf(fp, " ");
      dump_val(&inst->dst, fp);
      fprintf(fp, " ");
      dump_val(&inst->src, fp);
      break;
    case PUTC:
      fprintf(fp, " ");
      dump_val(&inst->src, fp);
      break;
    case GETC:
      fprintf(fp, " ");
      dump_val(&inst->dst, fp);
      break;
    case EXIT:
    case DUMP:
      break;
    case JEQ:
    case JNE:
    case JLT:
    case JGT:
    case JLE:
    case JGE:
      fprintf(fp, " ");
      dump_val(&inst->jmp, fp);
      fprintf(fp, " ");
      dump_val(&inst->dst, fp);
      fprintf(fp, " ");
      dump_val(&inst->src, fp);
      break;
    case JMP:
      fprintf(fp, " ");
      dump_val(&inst->jmp, fp);
      break;
    default:
      fprintf(fp, "oops op=%d\n", inst->op);
      exit(1);
  }
  fprintf(fp, " pc=%d @", inst->pc);
  int lineno = inst->lineno;
  // A hack to make the test for dump_ir.c.eir pass.
#ifndef __eir__
  lineno &= UINT_MAX;
#endif
  fprintf(fp, "%d\n", lineno);
}

void dump_inst(Inst* inst) {
  dump_inst_fp(inst, stderr);
}
