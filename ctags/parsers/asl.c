/*
*  geany_asl.c — ASL (ACPI Source Language) parser for Geany's bundled ctags.
*  Geany-maintained parser (not upstreamed to universal-ctags).
*  Tags: Device/Scope/Method blocks, nested by braces.
*/
#include "general.h"  /* must always come first */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "entry.h"
#include "parse.h"
#include "read.h"
#include "vstring.h"

typedef enum {
	K_NONE = -1,
	K_DEVICE,
	K_SCOPE,
	K_FIELD,
	K_METHOD,
	K_NAME,
	K_OBJECT,
	K_UNDEF,
	K_COUNT
} aslKind;

static kindDefinition AslKinds [] = {
	{ true, 'd', "device",      "devices" },
	{ true, 's', "scope",       "scopes"  },
	{ true, 'f', "field",       "fields"  },
	{ true, 'm', "method",      "methods" },
	{ true, 'n', "name",        "defined names" },
	{ true, 'o', "thermalzone", "other objects" },
	{ true, 'u', "processor",   "deprecated types" },
};

// scope stack: cork indexes of enclosing named scopes
static struct {
	int *ptr;
	unsigned int len, size;
} scopeStack = {
	NULL, 0,0
};

static inline int currentScope()
{
	return scopeStack.len ? scopeStack.ptr[scopeStack.len-1] : CORK_NIL;
}
static inline void scopeStackPush(int idx)
{
	if (scopeStack.len == scopeStack.size)
	{
		unsigned int cap = scopeStack.size ? scopeStack.size * 2 : 16;
		signed  int *tmp = realloc(scopeStack.ptr, cap * sizeof(int));

		if (tmp == NULL)
			return;  // out of memory: deeper scopes are just lost
		scopeStack.ptr  = tmp;
		scopeStack.size = cap;
	}
	scopeStack.ptr[scopeStack.len++] = idx;
}

// Skip whitespace and comments; return the next significant character.
static int skipToToken (int c)
{
	bool changed = true;

	while (changed && c != EOF)
	{
		changed = false;
		while (isspace (c))
			c = getcFromInputFile(), changed = true;
		if (c == '/')
		{
			int next = getcFromInputFile();

			if (next == '/') // line comment
			{
				while ((c = getcFromInputFile()) != EOF && c != '\n')
					;
				changed = true;
			}
			else if (next == '*') // block comment
			{
				int prev = 0;

				c = getcFromInputFile();
				while (c != EOF && ! (prev == '*' && c == '/'))
				{
					prev = c;
					c = getcFromInputFile();
				}
				if (c != EOF)
					c = getcFromInputFile();
				changed = true;
			}
			else if (next != EOF)
				ungetcToInputFile(next);
		}
	}
	return c;
}

// c is the opening quote; skip the string, return the char after the closing quote.
static int skipString(int c)
{
	c = getcFromInputFile();
	while (c != EOF && c != '"')
	{
		if (c == '\\')
			c = getcFromInputFile();
		if (c != EOF)
			c = getcFromInputFile();
	}
	return c == EOF ? EOF : getcFromInputFile();
}

/* Skip everything up to the ')' matching the already-consumed '('.
   Returns the matching ')' itself. */
static int skipParenHeader(int c)
{
	int depth = 1;

	while (c != EOF && depth > 0)
	{
		if (c == '"') {
			c = skipString (c);
		}
		else if (c == '/') 
		{
			int next = getcFromInputFile();

			if (next == '/') {
				while ((c = getcFromInputFile()) != EOF && c != '\n')
					;
			} else if (next == '*') {
				int prev = 0;

				c = getcFromInputFile();
				while (c != EOF && ! (prev == '*' && c == '/'))
				{
					prev = c;
					c = getcFromInputFile();
				}
				if (c != EOF)
					c = getcFromInputFile();
			} else if (next != EOF)
				c = next;
		}
		else if (c == '(')
		{
			depth++;
			c = getcFromInputFile();
		}
		else if (c == ')')
		{
			depth--;
			if (depth > 0)
				c = getcFromInputFile();
		} else
			c = getcFromInputFile();
	}
	return c;
}

// ASL names may carry path prefixes: \_SB.PCI0.GFX0, ^PCI0, ~GFX
#define isChrWord(_C) isalnum  (_C) || _C == '_'
#define isChrName(_C) isChrWord(_C) || _C == '~' || _C == '\\' || _C == '.' || _C == '^' 

// ASL keywords are case-insensitive; `keyword` must be lowercase.
static bool keywordIs(const vString *const word, const char *keyword)
{
	const char *s = vStringValue (word);
	signed i, len = vStringLength(word);

	for (i = 0; keyword[i] != '\0'; i++) {
		if (i >= len || tolower((unsigned char)s[i]) != keyword[i])
			return false;
	}
	return i == len;
}

/* Build the full ACPI-style path for a device: \_SB.PCI0.PX40.TPM.
   Walks the cork chain up to the first enclosing device (whose stored
   name is already a full path) or to the root collecting scope names. */
static void devicePath(int parentIdx, const vString *const name, vString *path)
{
	const char *segs[32];
	unsigned nsegs = 0;
	unsigned i;
	int idx = parentIdx;

	while (idx != CORK_NIL && nsegs < 32)
	{
		const tagEntryInfo *tag = getEntryInCorkQueue((unsigned long)idx);

		if (tag == NULL)
			break;
		segs[nsegs++] = tag->name;
		if (tag->kindIndex == K_DEVICE)
			break;              /* its name is already an absolute path */
		idx = tag->extensionFields.scopeIndex;
	}
	vStringClear(path);
	vStringPut  (path, '\\');

	for (i = 0; i < nsegs; i++)
	{
		const char *s = segs[nsegs - 1 - i];

		while (*s == '\\' || *s == '^' || *s == '~')
			s++;
		vStringCatS(path, s);
		if (i + 1 < nsegs)
			vStringCatS(path, ".");
	}
	vStringCatS(path, ".");
	vStringCat (path, name);
}


static int makeAslTag (const vString *const name, vString *path, aslKind kind)
{
	tagEntryInfo e;
	int corkIndex;

	if (kind == K_DEVICE) {
		devicePath(currentScope(), name, path);
		initTagEntry(&e, vStringValue(path), kind);
		e.extensionFields.scopeIndex = CORK_NIL;
		corkIndex = makeTagEntry(&e);
		vStringClear(path);
	} else if (
		kind == K_SCOPE && vStringLength(name) > 0 && vStringValue(name)[0] == '\\'
	) {
		/* Absolute path: Scope (\) is the root namespace — everything
		   declared inside is global. Scope (\_SB.PCI0) refers to an
		   object in the root namespace, not to a nested block. */
		if (vStringLength(name) == 1) {
			corkIndex = CORK_NIL; // root scope: no tag
		} else {
			initTagEntry(&e, vStringValue(name), kind);
			e.extensionFields.scopeIndex = CORK_NIL;
			corkIndex = makeTagEntry(&e);
		}
	} else {
		initTagEntry(&e, vStringValue(name), kind);
		e.extensionFields.scopeIndex = currentScope();
		corkIndex = makeTagEntry(&e);
	}
	return corkIndex;
}


static void findAslTags (void)
{
	vString *word = vStringNew();
	vString *name = vStringNew();
	vString *path = vStringNew();
	aslKind  kind = K_NONE;

	int c = getcFromInputFile();

	while (c != EOF)
	{
		if (c == '"') {
			c = skipString(c);
		}
		else if (c == '/')
		{
			int next = getcFromInputFile();

			if (next == '/')
			{
				while ((c = getcFromInputFile()) != EOF && c != '\n')
					;
			}
			else if (next == '*')
			{
				int prev = 0;

				c = getcFromInputFile();
				while (c != EOF && ! (prev == '*' && c == '/'))
				{
					prev = c;
					c = getcFromInputFile();
				}
				if (c != EOF)
					c = getcFromInputFile();
			} else
				c = next;  // ordinary character
		}
		else if (c == '{')
		{
			scopeStackPush(currentScope());  /* anonymous block inherits scope */
			c = getcFromInputFile();
		}
		else if (c == '}')
		{
			if (scopeStack.len > 0)
				scopeStack.len--;
			c = getcFromInputFile();
		}
		else if (isalpha ((unsigned char) c))
		{
			vStringClear(word);
			while (isChrWord(c)) {
				vStringPut(word, c);
				c = getcFromInputFile ();
			}
			vStringTruncate(word, vStringLength(word));

			kind = keywordIs(word, "device") ? K_DEVICE :
			       keywordIs(word, "scope" ) ? K_SCOPE  :
			       keywordIs(word, "method") ? K_METHOD :
			       keywordIs(word, "field" ) ? K_FIELD  :
			       keywordIs(word, "name"  ) ? K_NAME   :
			 keywordIs(word, "thermalzone" ) ? K_OBJECT :
			 keywordIs(word, "processor"   ) ? K_UNDEF  : K_NONE;

			c = skipToToken(c);

			if (kind != K_NONE && c == '(')
			{
				int corkIndex = currentScope();
				c = skipToToken(getcFromInputFile ());
				if (isChrName(c)) {
					vStringClear(name);
					while (isChrName(c)) {
						vStringPut(name, c);
						c = getcFromInputFile();
					}
					vStringTruncate(name, vStringLength(name));
					corkIndex = makeAslTag(name, path, kind);
				}
				// read the remainder of the header up to the closing ')' that follows the header
				c = skipParenHeader(c);
				c = skipToToken(getcFromInputFile());
				if (c == '{') {
					scopeStackPush(corkIndex);
					c = getcFromInputFile();
				}
			}
		} else
			c = getcFromInputFile();
	}
	vStringDelete(word);
	vStringDelete(name);
	vStringDelete(path);
	scopeStack.len = 0;
}

extern parserDefinition* AslParser(void)
{
	static const char *const extensions [] = { "asl", "dsl", NULL };
	parserDefinition* const def = parserNew("ASL");

	def->kindTable  = AslKinds;
	def->kindCount  = ARRAY_SIZE(AslKinds);
	def->parser     = findAslTags;
	def->extensions = extensions;
	def->useCork    = CORK_QUEUE;
	return def;
}
