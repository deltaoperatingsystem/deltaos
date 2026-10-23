#include <isoc/ctype.h>
#include <io.h>
#include <string.h>
#include <mem.h>

typedef enum {
    TOKEN_IDENT,
    TOKEN_NUMBER,
    TOKEN_EOF,
    TOKEN_BAD,
    TOKEN_STRING,
    TOKEN_CHAR,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_EQ,
    TOKEN_SEMI,
} TokenType;

typedef struct {
    char *stack;
    size cap;
    size idx;
} CharStack;

typedef struct {
    TokenType type;
    CharStack *data;
} Token;

char peek(CharStack *s) {
    return s->stack[s->idx];
}

char consume(CharStack *s) {
    return s->stack[s->idx++];
}

CharStack *charstack(size size) {
    CharStack *s = malloc(sizeof(CharStack));
    *s = (CharStack){
        .stack = malloc(size),
        .cap = size,
        .idx = 0
    };
    return s;
}

void charstack_push(CharStack *s, char c) {
    if (s->idx >= s->cap) {
        s->cap *= 2;
        s->stack = realloc(s->stack, s->cap);
    }
    s->stack[s->idx++] = c;
}

Token get_token(CharStack *src) {
    CharStack *buf = charstack(2);
    if (isalpha(peek(src))) {
        while (isalpha(peek(src))) {
            charstack_push(buf, consume(src));
        }

        return (Token){TOKEN_IDENT, buf};
    }
    if (isdigit(peek(src))) {
        while (isdigit(peek(src))) {
            charstack_push(buf, consume(src));
        }

        return (Token){TOKEN_NUMBER, buf};
    }
    if (peek(src) == '\0') return (Token){TOKEN_EOF, buf};
    if (isspace(peek(src))) {
        while (isspace(peek(src))) consume(src);
        free(buf->stack);
        free(buf);
        return get_token(src);
    }
    if (peek(src) == '#') {
        consume(src);
        while (peek(src) != '\n') consume(src);
        free(buf->stack);
        free(buf);
        return get_token(src);
    }
    if (peek(src) == '"') {
        consume(src);
        while (peek(src) != '"') charstack_push(buf, consume(src));
        consume(src);
        return (Token){TOKEN_STRING, buf};
    }
    if (peek(src) == '\'') {
        consume(src);
        while (peek(src) != '\'') charstack_push(buf, consume(src));
        consume(src);
        return (Token){TOKEN_CHAR, buf};
    }
    if (peek(src) == '{') {
        charstack_push(buf, consume(src));
        return (Token){TOKEN_LBRACE, buf};
    }
    if (peek(src) == '}') {
        charstack_push(buf, consume(src));
        return (Token){TOKEN_RBRACE, buf};
    }
    if (peek(src) == '=') {
        charstack_push(buf, consume(src));
        return (Token){TOKEN_EQ, buf};
    }
    if (peek(src) == ';') {
        charstack_push(buf, consume(src));
        return (Token){TOKEN_SEMI, buf};
    }


    consume(src);
    return (Token){TOKEN_BAD, buf};
}

typedef enum NodeType {
    NODE_BLOCK,
    NODE_PAIR,
} NodeType;

typedef struct Node {
    NodeType type;
    union {
        struct {
            char *tag;
            char *name;
            struct Node **children;
            size num_children;
            size cap;
        } block;
        struct {
            char *key, *value;
        } pair;
    };
} Node;

Node *parse_block(CharStack *src, Token t) {
    Node *n = malloc(sizeof(Node));
    n->type = NODE_BLOCK;
    
    Token next = get_token(src);
    if (next.type == TOKEN_STRING) n->block.tag = strdup(next.data->stack);

    next = get_token(src);
    if (next.type != TOKEN_LBRACE) exit(1);
}

Node *parse(CharStack *src) {
    Token t;
    do {
        t = get_token(src);
        switch (t.type) {
            case TOKEN_IDENT: return parse_block(src, t);
        }
    } while (t.type != TOKEN_EOF);
}

int main(int argc, char **argv) {
    static const char example[] = "# example configuration\nUsers {\n    user \"alice\" {\n        home = \"/\";\n        shell = \"/system/binaries/shell\";\n    }\n}";
    CharStack *src = charstack(sizeof(example));
    memcpy(src->stack, example, sizeof(example));
    Token t;
    do {
        t = get_token(src);
        printf("Token: %d %s\n", t.type, t.data->stack);
    } while (t.type != TOKEN_EOF);
}