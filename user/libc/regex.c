#include <ctype.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A pattern is parsed to a tree and compiled to a small program. Matching walks the
// program by backtracking and keeps the longest end it reaches. Without back
// references, reaching the same split at the same position twice can't lead anywhere
// new, so remembering those keeps the time at program size times text size.

#define MAX_PROGRAM 0x100000
#define MAX_DEPTH 500

enum node_kind {
    NODE_EMPTY, NODE_CHAR, NODE_ANY, NODE_SET, NODE_CAT, NODE_ALT, NODE_REPEAT, NODE_GROUP,
    NODE_BACKREF, NODE_ASSERT,
};

enum assertion { AT_LINE_START, AT_LINE_END, AT_WORD_EDGE, AT_NOT_WORD_EDGE, AT_WORD_START, AT_WORD_END, AT_TEXT_START, AT_TEXT_END };

struct node {
    enum node_kind kind;
    int value; // the character, set, group, assertion or lowest repeat
    int max; // the highest repeat, or -1 for no limit
    struct node *a;
    struct node *b;
    struct node *next_allocated;
};

enum op { OP_CHAR, OP_ANY, OP_SET, OP_SPLIT, OP_JUMP, OP_SAVE, OP_LOOP_CHECK, OP_BACKREF, OP_ASSERT, OP_MATCH };

struct inst {
    uint8_t op;
    int x; // the character, set, slot, assertion or first choice
    int y; // the second choice
    int split; // which split this is
};

struct set {
    uint8_t bits[32];
};

struct program {
    struct inst *code;
    struct set *sets;
    int length;
    int capacity;
    int set_count;
    int split_count;
    int loop_count;
    int group_count;
    int flags;
    bool has_backref;
};

struct parser {
    const char *at;
    int flags;
    int error;
    int depth;
    int open_groups;
    uint32_t closed_groups; // which of the first 32 have ended
    struct program *program;
    struct node *allocated;
};

static struct node *new_node(struct parser *p, enum node_kind kind, int value, struct node *a, struct node *b) {
    struct node *n = malloc(sizeof(*n));

    if (!n) {
        p->error = REG_ESPACE;
        return NULL;
    }
    *n = (struct node){.kind = kind, .value = value, .a = a, .b = b, .next_allocated = p->allocated};
    p->allocated = n;
    return n;
}

static void set_add(struct set *set, int c) {
    set->bits[c >> 3] |= 1 << (c & 7);
}

static void set_remove(struct set *set, int c) {
    set->bits[c >> 3] &= ~(1 << (c & 7));
}

static bool set_has(const struct set *set, int c) {
    return set->bits[c >> 3] >> (c & 7) & 1;
}

static int add_set(struct parser *p, const struct set *set) {
    struct program *prog = p->program;
    struct set *grown = realloc(prog->sets, (prog->set_count + 1) * sizeof(*grown));

    if (!grown) {
        p->error = REG_ESPACE;
        return 0;
    }
    prog->sets = grown;
    prog->sets[prog->set_count] = *set;
    return prog->set_count++;
}

static bool is_word(int c) {
    return isalnum(c) || c == '_';
}

static bool in_class(int c, const char *name, size_t len) {
    static const struct {
        const char *name;
        int (*test)(int);
    } classes[] = {
        {"alpha", isalpha}, {"digit", isdigit}, {"alnum", isalnum}, {"upper", isupper}, {"lower", islower},
        {"space", isspace}, {"blank", isblank}, {"punct", ispunct}, {"print", isprint}, {"graph", isgraph},
        {"cntrl", iscntrl}, {"xdigit", isxdigit},
    };

    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
        if (strlen(classes[i].name) == len && memcmp(classes[i].name, name, len) == 0)
            return classes[i].test(c);
    return false;
}

static bool known_class(const char *name, size_t len) {
    static const char *const names[] = {"alpha", "digit", "alnum", "upper", "lower", "space", "blank", "punct", "print", "graph", "cntrl", "xdigit"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i]) == len && memcmp(names[i], name, len) == 0)
            return true;
    return false;
}

// Applies the ignore-case and newline rules, then stores the set.
static struct node *finish_set(struct parser *p, struct set *set, bool negate) {
    if (p->flags & REG_ICASE) {
        for (int c = 'a'; c <= 'z'; c++) {
            if (set_has(set, c) || set_has(set, toupper(c))) {
                set_add(set, c);
                set_add(set, toupper(c));
            }
        }
    }
    if (negate) {
        for (int i = 0; i < 32; i++)
            set->bits[i] = ~set->bits[i];
        if (p->flags & REG_NEWLINE)
            set_remove(set, '\n');
    }
    return new_node(p, NODE_SET, add_set(p, set), NULL, NULL);
}

// One end of a range: a plain character, or [.x.] or [=x=]. Returns -1 on error.
static int bracket_char(struct parser *p) {
    if (p->at[0] == '[' && (p->at[1] == '.' || p->at[1] == '=')) {
        char kind = p->at[1];
        const char *c = p->at + 2;

        if (!c[0] || c[1] != kind || c[2] != ']') {
            p->error = c[0] && strchr(c, ']') ? REG_ECOLLATE : REG_EBRACK;
            return -1;
        }
        p->at = c + 3;
        return (unsigned char)*c;
    }
    return (unsigned char)*p->at++;
}

// Starts just after the opening bracket.
static struct node *parse_bracket(struct parser *p) {
    struct set set = {0};
    bool negate = *p->at == '^';
    bool first = true;

    if (negate)
        p->at++;

    while (first || *p->at != ']') {
        if (!*p->at) {
            p->error = REG_EBRACK;
            return NULL;
        }
        first = false;

        if (p->at[0] == '[' && p->at[1] == ':') {
            const char *name = p->at + 2;
            const char *end = strstr(name, ":]");

            if (!end) {
                p->error = REG_EBRACK;
                return NULL;
            }
            size_t len = end - name;

            if (!known_class(name, len)) {
                p->error = REG_ECTYPE;
                return NULL;
            }
            for (int c = 0; c < 256; c++)
                if (in_class(c, name, len))
                    set_add(&set, c);
            p->at = end + 2;
            continue;
        }

        int low = bracket_char(p);

        if (low < 0)
            return NULL;

        int high = low;

        if (p->at[0] == '-' && p->at[1] && p->at[1] != ']') {
            p->at++;
            high = bracket_char(p);
            if (high < 0)
                return NULL;
            if (high < low) {
                p->error = REG_ERANGE;
                return NULL;
            }
        }
        for (int c = low; c <= high; c++)
            set_add(&set, c);
    }
    p->at++;
    return finish_set(p, &set, negate);
}

static struct node *char_node(struct parser *p, int c) {
    if ((p->flags & REG_ICASE) && isalpha(c)) {
        struct set set = {0};

        set_add(&set, c);
        return finish_set(p, &set, false);
    }
    return new_node(p, NODE_CHAR, (unsigned char)c, NULL, NULL);
}

static struct node *class_node(struct parser *p, const char *name, bool negate) {
    struct set set = {0};

    for (int c = 0; c < 256; c++)
        if (name ? in_class(c, name, strlen(name)) : is_word(c))
            set_add(&set, c);
    return finish_set(p, &set, negate);
}

static struct node *parse_alternatives(struct parser *p);

static struct node *parse_group(struct parser *p, bool extended) {
    int group = ++p->program->group_count;

    p->open_groups++;

    struct node *inner = parse_alternatives(p);

    if (p->error)
        return NULL;
    if (extended ? *p->at != ')' : (p->at[0] != '\\' || p->at[1] != ')')) {
        p->error = REG_EPAREN;
        return NULL;
    }
    p->at += extended ? 1 : 2;
    p->open_groups--;
    if (group < 32)
        p->closed_groups |= 1u << group;
    return new_node(p, NODE_GROUP, group, inner, NULL);
}

// What follows a backslash, with at on the character after it.
static struct node *parse_escape(struct parser *p) {
    int c = (unsigned char)*p->at;

    if (!c) {
        p->error = REG_EESCAPE;
        return NULL;
    }
    p->at++;

    if (c >= '1' && c <= '9') {
        int group = c - '0';

        if (!(p->closed_groups >> group & 1)) {
            p->error = REG_ESUBREG;
            return NULL;
        }
        p->program->has_backref = true;
        return new_node(p, NODE_BACKREF, group, NULL, NULL);
    }

    switch (c) {
    case 'w':
        return class_node(p, NULL, false);
    case 'W':
        return class_node(p, NULL, true);
    case 's':
        return class_node(p, "space", false);
    case 'S':
        return class_node(p, "space", true);
    case 'b':
        return new_node(p, NODE_ASSERT, AT_WORD_EDGE, NULL, NULL);
    case 'B':
        return new_node(p, NODE_ASSERT, AT_NOT_WORD_EDGE, NULL, NULL);
    case '<':
        return new_node(p, NODE_ASSERT, AT_WORD_START, NULL, NULL);
    case '>':
        return new_node(p, NODE_ASSERT, AT_WORD_END, NULL, NULL);
    case '`':
        return new_node(p, NODE_ASSERT, AT_TEXT_START, NULL, NULL);
    case '\'':
        return new_node(p, NODE_ASSERT, AT_TEXT_END, NULL, NULL);
    default:
        return char_node(p, c);
    }
}

static bool at_branch_end(const struct parser *p, bool extended) {
    const char *s = p->at;

    if (!*s)
        return true;
    if (extended)
        return *s == '|' || (*s == ')' && p->open_groups > 0);
    return s[0] == '\\' && (s[1] == '|' || s[1] == ')');
}

// One character, set, group or anchor, with no repeat. at_start says nothing comes
// before it in this branch, which is what makes ^ an anchor in a basic expression.
static struct node *parse_atom(struct parser *p, bool at_start) {
    bool extended = p->flags & REG_EXTENDED;
    char c = *p->at;

    if (c == '[') {
        p->at++;
        return parse_bracket(p);
    }
    if (c == '.') {
        p->at++;
        if (!(p->flags & REG_NEWLINE))
            return new_node(p, NODE_ANY, 0, NULL, NULL);

        struct set set = {0};

        set_add(&set, '\n');
        return finish_set(p, &set, true);
    }
    if (c == '\\') {
        if (!extended && p->at[1] == '(') {
            p->at += 2;
            return parse_group(p, false);
        }
        if (!extended && p->at[1] == ')') {
            p->error = REG_EPAREN;
            return NULL;
        }
        if (!extended && p->at[1] == '{' && at_start) {
            p->error = REG_BADRPT;
            return NULL;
        }
        p->at++;
        return parse_escape(p);
    }
    if (c == '^' && (extended || at_start)) {
        p->at++;
        return new_node(p, NODE_ASSERT, AT_LINE_START, NULL, NULL);
    }
    if (c == '$') {
        p->at++;
        if (extended || at_branch_end(p, false))
            return new_node(p, NODE_ASSERT, AT_LINE_END, NULL, NULL);
        return char_node(p, '$');
    }
    if (extended && c == '(') {
        p->at++;
        return parse_group(p, true);
    }
    if (extended && at_start && (c == '*' || c == '+' || c == '?' || c == '{')) {
        p->error = REG_BADRPT;
        return NULL;
    }
    p->at++;
    return char_node(p, c);
}

// Reads the numbers of an interval, with at on the first digit. Returns false if it
// isn't one, leaving the reason in error.
static bool parse_interval(struct parser *p, bool extended, int *low, int *high) {
    const char *s = p->at;
    long a = -1;
    long b;

    if (isdigit((unsigned char)*s))
        a = strtol(s, (char **)&s, 10);
    b = a;
    if (*s == ',') {
        s++;
        b = isdigit((unsigned char)*s) ? strtol(s, (char **)&s, 10) : -1;
        if (a < 0)
            a = 0;
    }
    if (a < 0) {
        p->error = *s && !extended ? REG_BADBR : REG_EBRACE;
        return false;
    }
    if (extended ? *s != '}' : (s[0] != '\\' || s[1] != '}')) {
        p->error = *s ? REG_BADBR : REG_EBRACE;
        return false;
    }
    if (b >= 0 && b < a) {
        p->error = REG_BADBR;
        return false;
    }
    if (a > RE_DUP_MAX || b > RE_DUP_MAX) {
        p->error = REG_ESIZE;
        return false;
    }
    p->at = s + (extended ? 1 : 2);
    *low = a;
    *high = b;
    return true;
}

static struct node *repeat_node(struct parser *p, struct node *atom, int low, int high) {
    struct node *n = new_node(p, NODE_REPEAT, low, atom, NULL);

    if (n)
        n->max = high;
    return n;
}

static struct node *parse_repeats(struct parser *p, struct node *atom) {
    bool extended = p->flags & REG_EXTENDED;

    while (atom && !p->error) {
        const char *s = p->at;

        // Only an extended expression sees a repeat here as having nothing to repeat.
        if (atom->kind == NODE_ASSERT && atom->value == AT_LINE_START) {
            if (extended && (*s == '*' || *s == '+' || *s == '?' || *s == '{'))
                p->error = REG_BADRPT;
            return atom;
        }
        if (*s == '*') {
            p->at++;
            atom = repeat_node(p, atom, 0, -1);
        } else if (extended && *s == '+') {
            p->at++;
            atom = repeat_node(p, atom, 1, -1);
        } else if (extended && *s == '?') {
            p->at++;
            atom = repeat_node(p, atom, 0, 1);
        } else if (!extended && s[0] == '\\' && s[1] == '+') {
            p->at += 2;
            atom = repeat_node(p, atom, 1, -1);
        } else if (!extended && s[0] == '\\' && s[1] == '?') {
            p->at += 2;
            atom = repeat_node(p, atom, 0, 1);
        } else if (extended && *s == '{') {
            int low, high;

            p->at++;
            if (!parse_interval(p, true, &low, &high))
                return NULL;
            atom = repeat_node(p, atom, low, high);
        } else if (!extended && s[0] == '\\' && s[1] == '{') {
            int low, high;

            p->at += 2;
            if (!parse_interval(p, false, &low, &high))
                return NULL;
            atom = repeat_node(p, atom, low, high);
        } else {
            break;
        }
    }
    return atom;
}

static struct node *parse_branch(struct parser *p) {
    bool extended = p->flags & REG_EXTENDED;
    struct node *whole = NULL;
    bool at_start = true;

    while (!p->error && !at_branch_end(p, extended)) {
        struct node *atom;

        // In a basic expression a star that starts a branch, or follows the anchor
        // that does, is an ordinary character.
        if (!extended && *p->at == '*' && (at_start || (whole && whole->kind == NODE_ASSERT && whole->value == AT_LINE_START))) {
            p->at++;
            atom = char_node(p, '*');
        } else {
            atom = parse_atom(p, at_start);
        }
        atom = parse_repeats(p, atom);
        if (p->error)
            return NULL;
        whole = whole ? new_node(p, NODE_CAT, 0, whole, atom) : atom;
        at_start = false;
    }
    return whole ? whole : new_node(p, NODE_EMPTY, 0, NULL, NULL);
}

static struct node *parse_alternatives(struct parser *p) {
    bool extended = p->flags & REG_EXTENDED;

    if (++p->depth > MAX_DEPTH) {
        p->error = REG_ESPACE;
        return NULL;
    }

    struct node *whole = parse_branch(p);

    while (!p->error && (extended ? *p->at == '|' : (p->at[0] == '\\' && p->at[1] == '|'))) {
        p->at += extended ? 1 : 2;

        struct node *next = parse_branch(p);

        if (p->error)
            return NULL;
        whole = new_node(p, NODE_ALT, 0, whole, next);
    }
    p->depth--;
    return whole;
}

// Returns where the instruction went, or -1 with the error set.
static int emit(struct parser *p, enum op op, int x, int y) {
    struct program *prog = p->program;

    if (prog->length == MAX_PROGRAM) {
        p->error = REG_ESPACE;
        return -1;
    }
    if (prog->length == prog->capacity) {
        int capacity = prog->capacity ? prog->capacity * 2 : 32;
        struct inst *grown = realloc(prog->code, capacity * sizeof(*grown));

        if (!grown) {
            p->error = REG_ESPACE;
            return -1;
        }
        prog->code = grown;
        prog->capacity = capacity;
    }
    prog->code[prog->length] = (struct inst){.op = op, .x = x, .y = y};
    return prog->length++;
}

static int emit_split(struct parser *p) {
    int at = emit(p, OP_SPLIT, 0, 0);

    if (at >= 0)
        p->program->code[at].split = p->program->split_count++;
    return at;
}

static void generate(struct parser *p, const struct node *n) {
    struct program *prog = p->program;

    if (p->error)
        return;

    switch (n->kind) {
    case NODE_EMPTY:
        break;
    case NODE_CHAR:
        emit(p, OP_CHAR, n->value, 0);
        break;
    case NODE_ANY:
        emit(p, OP_ANY, 0, 0);
        break;
    case NODE_SET:
        emit(p, OP_SET, n->value, 0);
        break;
    case NODE_CAT:
        generate(p, n->a);
        generate(p, n->b);
        break;
    case NODE_ALT: {
        int split = emit_split(p);

        generate(p, n->a);

        int jump = emit(p, OP_JUMP, 0, 0);

        if (p->error)
            return;
        prog->code[split].x = split + 1;
        prog->code[split].y = prog->length;
        generate(p, n->b);
        if (!p->error)
            prog->code[jump].x = prog->length;
        break;
    }
    case NODE_GROUP:
        emit(p, OP_SAVE, n->value * 2, 0);
        generate(p, n->a);
        emit(p, OP_SAVE, n->value * 2 + 1, 0);
        break;
    case NODE_BACKREF:
        emit(p, OP_BACKREF, n->value, 0);
        break;
    case NODE_ASSERT:
        emit(p, OP_ASSERT, n->value, 0);
        break;
    case NODE_REPEAT:
        for (int i = 0; i < n->value && !p->error; i++)
            generate(p, n->a);

        if (n->max < 0) {
            int slot = prog->loop_count++;
            int split = emit_split(p);

            emit(p, OP_SAVE, -1 - slot, 0);
            generate(p, n->a);
            emit(p, OP_LOOP_CHECK, slot, 0);
            emit(p, OP_JUMP, split, 0);
            if (!p->error) {
                prog->code[split].x = split + 1;
                prog->code[split].y = prog->length;
            }
        } else {
            int count = n->max - n->value;
            int *splits = malloc((count + 1) * sizeof(*splits));

            if (!splits) {
                p->error = REG_ESPACE;
                return;
            }
            for (int i = 0; i < count && !p->error; i++) {
                splits[i] = emit_split(p);
                generate(p, n->a);
            }
            if (!p->error) {
                for (int i = 0; i < count; i++) {
                    prog->code[splits[i]].x = splits[i] + 1;
                    prog->code[splits[i]].y = prog->length;
                }
            }
            free(splits);
        }
        break;
    }
}

int regcomp(regex_t *regex, const char *pattern, int flags) {
    struct program *prog = calloc(1, sizeof(*prog));

    if (!prog)
        return REG_ESPACE;
    prog->flags = flags;

    struct parser p = {.at = pattern, .flags = flags, .program = prog};
    struct node *tree = parse_alternatives(&p);

    if (!p.error && *p.at)
        p.error = REG_EPAREN;
    if (!p.error) {
        generate(&p, tree);
        emit(&p, OP_MATCH, 0, 0);
    }

    while (p.allocated) {
        struct node *next = p.allocated->next_allocated;

        free(p.allocated);
        p.allocated = next;
    }

    regex->compiled = prog;
    regex->re_nsub = prog->group_count;
    if (p.error) {
        regfree(regex);
        return p.error;
    }
    return 0;
}

void regfree(regex_t *regex) {
    struct program *prog = regex->compiled;

    if (!prog)
        return;
    free(prog->code);
    free(prog->sets);
    free(prog);
    regex->compiled = NULL;
}

struct frame {
    int pc; // or a slot to restore, as -1 - slot
    int pos; // or the value to put back
};

struct matcher {
    const struct program *prog;
    const char *text;
    int length;
    int flags;
    int *slots;
    int slot_count;
    int *best;
    int best_end;
    struct frame *stack;
    int depth;
    int capacity;
    uint8_t *visited;
};

static bool push(struct matcher *m, int pc, int pos) {
    if (m->depth == m->capacity) {
        int capacity = m->capacity ? m->capacity * 2 : 64;
        struct frame *grown = realloc(m->stack, capacity * sizeof(*grown));

        if (!grown)
            return false;
        m->stack = grown;
        m->capacity = capacity;
    }
    m->stack[m->depth++] = (struct frame){pc, pos};
    return true;
}

static bool word_at(const struct matcher *m, int pos) {
    return pos >= 0 && pos < m->length && is_word((unsigned char)m->text[pos]);
}

static bool assertion_holds(const struct matcher *m, int which, int pos) {
    bool newline = m->prog->flags & REG_NEWLINE;

    switch (which) {
    case AT_LINE_START:
        if (pos == 0 && !(m->flags & REG_NOTBOL))
            return true;
        return newline && pos > 0 && m->text[pos - 1] == '\n';
    case AT_LINE_END:
        if (pos == m->length && !(m->flags & REG_NOTEOL))
            return true;
        return newline && pos < m->length && m->text[pos] == '\n';
    case AT_WORD_EDGE:
        return word_at(m, pos - 1) != word_at(m, pos);
    case AT_NOT_WORD_EDGE:
        return word_at(m, pos - 1) == word_at(m, pos);
    case AT_WORD_START:
        return !word_at(m, pos - 1) && word_at(m, pos);
    case AT_WORD_END:
        return word_at(m, pos - 1) && !word_at(m, pos);
    case AT_TEXT_START:
        return pos == 0 && !(m->flags & REG_NOTBOL);
    case AT_TEXT_END:
        return pos == m->length && !(m->flags & REG_NOTEOL);
    }
    return false;
}

static bool backref_matches(const struct matcher *m, int group, int pos, int *end) {
    int start = m->slots[group * 2];
    int stop = m->slots[group * 2 + 1];

    if (start < 0 || stop < 0)
        return false;

    int len = stop - start;

    if (len > m->length - pos)
        return false;
    for (int i = 0; i < len; i++) {
        unsigned char a = m->text[start + i];
        unsigned char b = m->text[pos + i];

        if (a != b && !((m->prog->flags & REG_ICASE) && tolower(a) == tolower(b)))
            return false;
    }
    *end = pos + len;
    return true;
}

// Tries a match at one start. Returns 0 or REG_ESPACE; best_end says if it matched.
static int run(struct matcher *m, int start) {
    const struct inst *code = m->prog->code;
    int group_slots = (m->prog->group_count + 1) * 2;

    for (int i = 0; i < m->slot_count; i++)
        m->slots[i] = -1;
    m->slots[0] = start;
    m->depth = 0;

    int pc = 0;
    int pos = start;

    for (;;) {
        const struct inst *in = &code[pc];
        bool failed = false;

        switch (in->op) {
        case OP_CHAR:
            if (pos < m->length && (unsigned char)m->text[pos] == in->x) {
                pos++;
                pc++;
            } else {
                failed = true;
            }
            break;
        case OP_ANY:
            if (pos < m->length) {
                pos++;
                pc++;
            } else {
                failed = true;
            }
            break;
        case OP_SET:
            if (pos < m->length && set_has(&m->prog->sets[in->x], (unsigned char)m->text[pos])) {
                pos++;
                pc++;
            } else {
                failed = true;
            }
            break;
        case OP_SPLIT:
            if (m->visited) {
                size_t bit = (size_t)in->split * (m->length + 1) + pos;

                if (m->visited[bit >> 3] >> (bit & 7) & 1) {
                    failed = true;
                    break;
                }
                m->visited[bit >> 3] |= 1 << (bit & 7);
            }
            if (!push(m, in->y, pos))
                return REG_ESPACE;
            pc = in->x;
            break;
        case OP_JUMP:
            pc = in->x;
            break;
        case OP_SAVE: {
            // Negative numbers are where a loop's turn began.
            int slot = in->x < 0 ? group_slots + (-1 - in->x) : in->x;

            if (!push(m, -1 - slot, m->slots[slot]))
                return REG_ESPACE;
            m->slots[slot] = pos;
            pc++;
            break;
        }
        case OP_LOOP_CHECK:
            // A turn that used up nothing would go round forever.
            if (m->slots[group_slots + in->x] == pos)
                failed = true;
            else
                pc++;
            break;
        case OP_BACKREF: {
            int end;

            if (backref_matches(m, in->x, pos, &end)) {
                pos = end;
                pc++;
            } else {
                failed = true;
            }
            break;
        }
        case OP_ASSERT:
            if (assertion_holds(m, in->x, pos))
                pc++;
            else
                failed = true;
            break;
        case OP_MATCH:
            if (pos > m->best_end) {
                m->best_end = pos;
                memcpy(m->best, m->slots, group_slots * sizeof(int));
                m->best[1] = pos;
            }
            if (pos == m->length)
                return 0;
            failed = true;
            break;
        }

        if (!failed)
            continue;

        // Go back to the last choice, undoing what was saved since.
        for (;;) {
            if (m->depth == 0)
                return 0;

            struct frame f = m->stack[--m->depth];

            if (f.pc >= 0) {
                pc = f.pc;
                pos = f.pos;
                break;
            }
            m->slots[-1 - f.pc] = f.pos;
        }
    }
}

int regexec(const regex_t *regex, const char *text, size_t nmatch, regmatch_t *matches, int flags) {
    const struct program *prog = regex->compiled;
    int first = 0;
    size_t length;

    if (flags & REG_STARTEND) {
        first = matches[0].rm_so;
        length = matches[0].rm_eo;
    } else {
        length = strlen(text);
    }
    if (length > INT32_MAX)
        return REG_ESPACE;

    int group_slots = (prog->group_count + 1) * 2;
    struct matcher m = {
        .prog = prog,
        .text = text,
        .length = length,
        .flags = flags,
        .slot_count = group_slots + prog->loop_count,
        .best_end = -1,
    };
    int status = REG_ESPACE;

    m.slots = malloc(m.slot_count * sizeof(int));
    m.best = malloc(group_slots * sizeof(int));
    if (!m.slots || !m.best)
        goto done;

    if (!prog->has_backref && prog->split_count) {
        m.visited = calloc(((size_t)prog->split_count * (length + 1) + 7) / 8 + 1, 1);
        if (!m.visited)
            goto done;
    }

    status = REG_NOMATCH;
    for (int start = first; start <= (int)length; start++) {
        int result = run(&m, start);

        if (result) {
            status = result;
            goto done;
        }
        if (m.best_end >= 0) {
            status = 0;
            break;
        }
    }

    if (status == 0 && !(prog->flags & REG_NOSUB)) {
        for (size_t i = 0; i < nmatch; i++) {
            bool have = i <= (size_t)prog->group_count && m.best[i * 2] >= 0 && m.best[i * 2 + 1] >= 0;

            matches[i].rm_so = have ? m.best[i * 2] : -1;
            matches[i].rm_eo = have ? m.best[i * 2 + 1] : -1;
        }
    }

done:
    free(m.slots);
    free(m.best);
    free(m.stack);
    free(m.visited);
    return status;
}

size_t regerror(int code, const regex_t *regex, char *out, size_t size) {
    static const char *const messages[] = {
        "Success", "No match", "Invalid regular expression", "Invalid collation character",
        "Invalid character class name", "Trailing backslash", "Invalid back reference",
        "Unmatched [", "Unmatched ( or )", "Unmatched { or }", "Invalid content of {}",
        "Invalid range end", "Out of memory", "Invalid preceding regular expression",
        "Premature end of regular expression", "Regular expression too big",
    };
    const char *message = code >= 0 && code <= REG_ESIZE ? messages[code] : "Unknown error";
    size_t len = strlen(message);

    (void)regex;
    if (size) {
        size_t copied = len < size - 1 ? len : size - 1;

        memcpy(out, message, copied);
        out[copied] = '\0';
    }
    return len + 1;
}
