/*
 * HTML backend for Halibut
 */

/*
 * TODO:
 * 
 *  - I'm never entirely convinced that having a fragment link to
 *    come in at the start of the real text in the file is
 *    sensible. Perhaps for the topmost section in the file, no
 *    fragment should be used? (Though it should probably still be
 *    _there_ even if unused.)
 * 
 *  - In HHK index mode: subsidiary hhk entries (as in replacing
 *    `foo, bar' with `foo\n\tbar') can be done by embedding
 *    sub-<UL>s in the hhk file. This requires me getting round to
 *    supporting that idiom in the rest of Halibut, but I thought
 *    I'd record how it's done here in case I turn out to have
 *    forgotten when I get there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include "halibut.h"
#include "winchm.h"

#define is_heading_type(type) ( (type) == para_Title || \
				(type) == para_Chapter || \
				(type) == para_Appendix || \
				(type) == para_UnnumberedChapter || \
				(type) == para_Heading || \
				(type) == para_Subsect)

#define heading_depth(p) ( (p)->type == para_Subsect ? (p)->aux + 1 : \
			   (p)->type == para_Heading ? 1 : \
			   (p)->type == para_Title ? -1 : 0 )

typedef struct {
    bool number_at_all, just_numbers;
    wchar_t *number_suffix;
} sectlevel;

typedef struct {
    int nasect;
    sectlevel achapter, *asect;
    int *contents_depths;	       /* 0=main, 1=chapter, 2=sect etc */
    int ncdepths;
    bool address_section, visible_version_id;
    bool leaf_contains_contents;
    int leaf_smallest_contents;
    bool navlinks;
    bool rellinks;
    char *contents_filename;
    char *index_filename;
    char *template_filename;
    char *single_filename;
    char *chm_filename, *hhp_filename, *hhc_filename, *hhk_filename;
    char **template_fragments;
    int ntfragments;
    char **chm_extrafiles, **chm_extranames;
    int nchmextrafiles, chmextrafilesize;
    char *head_end, *body_start, *body_end, *addr_start, *addr_end;
    char *body_tag, *nav_attr;
    wchar_t *author, *description;
    wchar_t *index_text, *contents_text, *preamble_text, *title_separator;
    wchar_t *nav_prev_text, *nav_next_text, *nav_up_text, *nav_separator;
    wchar_t *index_main_sep, *index_multi_sep;
    wchar_t *pre_versionid, *post_versionid;
    int restrict_charset, output_charset;
    enum {
	HTML_3_2, HTML_4, ISO_HTML,
	XHTML_1_0_TRANSITIONAL, XHTML_1_0_STRICT
    } htmlver;
    wchar_t *lquote, *rquote;
    int leaf_level;
} htmlconfig;

#define contents_depth(conf, level) \
    ( (conf).ncdepths > (level) ? (conf).contents_depths[level] : (level)+2 )

#define is_xhtml(ver) ((ver) >= XHTML_1_0_TRANSITIONAL)

typedef struct htmlfile htmlfile;
typedef struct htmlsect htmlsect;

struct htmlfile {
    htmlfile *next;
    char *filename;
    int last_fragment_number;
    int min_heading_depth;
    htmlsect *first, *last;	       /* first/last highest-level sections */
    /*
     * The `temp' field is available for use in individual passes
     * over the file list. For example, the HHK index generation
     * uses it to ensure no index term references the same file
     * more than once.
     */
    int temp;
    /*
     * CHM section structure, if we're generating a CHM.
     */
    struct chm_section *chmsect;
};

struct htmlsect {
    htmlsect *next, *parent;
    htmlfile *file;
    paragraph *title, *text;
    enum { NORMAL, TOP, INDEX } type;
    int contents_depth;
    char **fragments;
};

typedef struct {
    htmlfile *head, *tail;
    htmlfile *single, *index;
    tree234 *frags;
    tree234 *files;
} htmlfilelist;

typedef struct {
    htmlsect *head, *tail;
} htmlsectlist;

typedef struct {
    htmlfile *file;
    char *fragment;
} htmlfragment;

typedef struct {
    int nrefs, refsize;
    word **refs;
} htmlindex;

typedef struct {
    htmlsect *section;
    char *fragment;
    bool generated, referenced;
} htmlindexref;

typedef struct {
    /*
     * This level deals with charset conversion, starting and
     * ending tags, and writing to the file. It's the lexical
     * level.
     */
    void *write_ctx;
    void (*write)(void *write_ctx, const char *data, int len);
    int charset, restrict_charset;
    charset_state cstate;
    errorstate *es;
    int ver;
    enum {
	HO_NEUTRAL, HO_IN_TAG, HO_IN_EMPTY_TAG, HO_IN_TEXT
    } state;
    int hackflags;		       /* used for icky .HH* stuff */
    int hacklimit;		       /* text size limit, again for .HH* */
    /*
     * Stuff beyond here deals with the higher syntactic level: it
     * tracks how many levels of <ul> are currently open when
     * producing a contents list, for example.
     */
    int contents_level;
} htmloutput;

void ho_write_ignore(void *write_ctx, const char *data, int len)
{
    IGNORE(write_ctx);
    IGNORE(data);
    IGNORE(len);
}
void ho_write_file(void *write_ctx, const char *data, int len)
{
    FILE *fp = (FILE *)write_ctx;
    if (len == -1)
        fclose(fp);
    else
        fwrite(data, 1, len, fp);
}
void ho_write_stdio(void *write_ctx, const char *data, int len)
{
    /* same as write_file, but we don't close the file */
    FILE *fp = (FILE *)write_ctx;
    if (len > 0)
        fwrite(data, 1, len, fp);
}
void ho_setup_file(htmloutput *ho, const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (fp) {
        ho->write = ho_write_file;
        ho->write_ctx = fp;
    } else {
        err_cantopenw(ho->es, filename);
        ho->write = ho_write_ignore; /* saves conditionalising rest of code */
    }
}
void ho_setup_stdio(htmloutput *ho, FILE *fp)
{
    ho->write = ho_write_stdio;
    ho->write_ctx = fp;
}

struct chm_output {
    struct chm *chm;
    char *filename;
    rdstringc rs;
};
void ho_write_chm(void *write_ctx, const char *data, int len)
{
    struct chm_output *co = (struct chm_output *)write_ctx;
    if (len == -1) {
        chm_add_file(co->chm, co->filename, co->rs.text, co->rs.pos);
        sfree(co->filename);
        sfree(co->rs.text);
        sfree(co);
    } else {
        rdaddsn(&co->rs, data, len);
    }
}
void ho_setup_chm(htmloutput *ho, struct chm *chm, const char *filename)
{
    struct chm_output *co = snew(struct chm_output);

    co->chm = chm;
    co->rs = empty_rdstringc;
    co->filename = dupstr(filename);

    ho->write_ctx = co;
    ho->write = ho_write_chm;
}

void ho_write_rdstringc(void *write_ctx, const char *data, int len)
{
    rdstringc *rs = (rdstringc *)write_ctx;
    if (len > 0)
        rdaddsn(rs, data, len);
}
void ho_setup_rdstringc(htmloutput *ho, rdstringc *rs)
{
    ho->write_ctx = rs;
    ho->write = ho_write_rdstringc;
}

void ho_string(htmloutput *ho, const char *string)
{
    ho->write(ho->write_ctx, string, strlen(string));
}
void ho_finish(htmloutput *ho)
{
    ho->write(ho->write_ctx, NULL, -1);
}

/*
 * Nasty hacks that modify the behaviour of htmloutput files. All
 * of these are flag bits set in ho.hackflags. HO_HACK_QUOTEQUOTES
 * has the same effect as the `quote_quotes' parameter to
 * html_text_limit_internal, except that it's set globally on an
 * entire htmloutput structure; HO_HACK_QUOTENOTHING suppresses
 * quoting of any HTML special characters (for .HHP files);
 * HO_HACK_OMITQUOTES completely suppresses the generation of
 * double quotes at all (turning them into single quotes, for want
 * of a better idea).
 */
#define HO_HACK_QUOTEQUOTES 1
#define HO_HACK_QUOTENOTHING 2
#define HO_HACK_OMITQUOTES 4

static int html_fragment_compare(const void *av, const void *bv, void *cmpctx)
{
    const htmlfragment *a = (const htmlfragment *)av;
    const htmlfragment *b = (const htmlfragment *)bv;
    int cmp;

    if ((cmp = strcmp(a->file->filename, b->file->filename)) != 0)
	return cmp;
    else
	return strcmp(a->fragment, b->fragment);
}

static int html_filename_compare(const void *av, const void *bv, void *cmpctx)
{
    const char *a = (const char *)av;
    const char *b = (const char *)bv;

    return strcmp(a, b);
}

static void html_file_section(htmlconfig *cfg, htmlfilelist *files,
			      htmlsect *sect, int depth);

static htmlfile *html_new_file(htmlfilelist *list, char *filename);
static htmlsect *html_new_sect(htmlsectlist *list, paragraph *title,
			       htmlconfig *cfg);

/* Flags for html_words() flags parameter */
#define NOTHING 0x00
#define MARKUP 0x01
#define LINKS 0x02
#define INDEXENTS 0x04
#define ALL 0x07
static void html_words(htmloutput *ho, word *words, int flags,
		       htmlfile *file, keywordlist *keywords, htmlconfig *cfg);
static void html_codepara(htmloutput *ho, word *words);

static void element_open(htmloutput *ho, char const *name);
static void element_close(htmloutput *ho, char const *name);
static void element_empty(htmloutput *ho, char const *name);
static void element_attr(htmloutput *ho, char const *name, char const *value);
static void element_attr_w(htmloutput *ho, char const *name,
			   wchar_t const *value);
static void html_text(htmloutput *ho, wchar_t const *str);
static void html_text_nbsp(htmloutput *ho, wchar_t const *str);
static void html_text_limit(htmloutput *ho, wchar_t const *str, int maxlen);
static void html_text_limit_internal(htmloutput *ho, wchar_t const *text,
				     int maxlen, bool quote_quotes, bool nbsp);
static void html_nl(htmloutput *ho);
static void html_raw(htmloutput *ho, char *text);
static void html_raw_as_attr(htmloutput *ho, char *text);
static void cleanup(htmloutput *ho);

static void html_href(htmloutput *ho, htmlfile *thisfile,
		      htmlfile *targetfile, char *targetfrag);
static void html_fragment(htmloutput *ho, char const *fragment);

static char *html_format(paragraph *p, char *template_string);
static char *html_sanitise_fragment(htmlfilelist *files, htmlfile *file,
				    char *text);
static char *html_sanitise_filename(htmlfilelist *files, char *text);

static void html_contents_entry(htmloutput *ho, int depth, htmlsect *s,
				htmlfile *thisfile, keywordlist *keywords,
				htmlconfig *cfg);
static void html_section_title(htmloutput *ho, htmlsect *s,
			       htmlfile *thisfile, keywordlist *keywords,
			       htmlconfig *cfg, bool real);

static htmlconfig html_configure(paragraph *source, bool chm_mode,
                                 errorstate *es)
{
    htmlconfig ret;
    paragraph *p;

    /*
     * Defaults.
     */
    ret.leaf_level = chm_mode ? -1 /* infinite */ : 2;
    ret.achapter.just_numbers = false;
    ret.achapter.number_at_all = true;
    ret.achapter.number_suffix = L": ";
    ret.nasect = 1;
    ret.asect = snewn(ret.nasect, sectlevel);
    ret.asect[0].just_numbers = true;
    ret.asect[0].number_at_all = true;
    ret.asect[0].number_suffix = L" ";
    ret.ncdepths = 0;
    ret.contents_depths = 0;
    ret.visible_version_id = true;
    ret.address_section = chm_mode ? false : true;
    ret.leaf_contains_contents = false;
    ret.leaf_smallest_contents = 4;
    ret.navlinks = chm_mode ? false : true;
    ret.rellinks = true;
    ret.single_filename = dupstr("Manual.html");
    ret.contents_filename = dupstr("Contents.html");
    ret.index_filename = dupstr("IndexPage.html");
    ret.template_filename = dupstr("%n.html");
    if (chm_mode) {
        ret.chm_filename = dupstr("output.chm");
        ret.hhc_filename = dupstr("contents.hhc");
        ret.hhk_filename = dupstr("index.hhk");
        ret.hhp_filename = NULL;
    } else {
        ret.chm_filename = ret.hhp_filename = NULL;
        ret.hhc_filename = ret.hhk_filename = NULL;
    }
    ret.ntfragments = 1;
    ret.template_fragments = snewn(ret.ntfragments, char *);
    ret.template_fragments[0] = dupstr("%b");
    ret.chm_extrafiles = ret.chm_extranames = NULL;
    ret.nchmextrafiles = ret.chmextrafilesize = 0;
    ret.head_end = ret.body_tag = ret.body_start = ret.body_end =
	ret.addr_start = ret.addr_end = ret.nav_attr = NULL;
    ret.author = ret.description = NULL;
    ret.restrict_charset = CS_UTF8;
    ret.output_charset = CS_ASCII;
    ret.htmlver = HTML_4;
    ret.index_text = L"Index";
    ret.contents_text = L"Contents";
    ret.preamble_text = L"Preamble";
    ret.title_separator = L" - ";
    ret.nav_prev_text = L"Previous";
    ret.nav_next_text = L"Next";
    ret.nav_up_text = L"Up";
    ret.nav_separator = L" | ";
    ret.index_main_sep = L": ";
    ret.index_multi_sep = L", ";
    ret.pre_versionid = L"[";
    ret.post_versionid = L"]";
    /*
     * Default quote characters are Unicode matched single quotes,
     * falling back to ordinary ASCII ".
     */
    ret.lquote = L"\x2018\0\x2019\0\"\0\"\0\0";
    ret.rquote = uadv(ret.lquote);

    /*
     * Two-pass configuration so that we can pick up global config
     * (e.g. `quotes') before having it overridden by specific
     * config (`html-quotes'), irrespective of the order in which
     * they occur.
     */
    for (p = source; p; p = p->next) {
	if (p->type == para_Config) {
	    if (!ustricmp(p->keyword, L"quotes")) {
		if (*uadv(p->keyword) && *uadv(uadv(p->keyword))) {
		    ret.lquote = uadv(p->keyword);
		    ret.rquote = uadv(ret.lquote);
		}
	    } else if (!ustricmp(p->keyword, L"index")) {
		ret.index_text = uadv(p->keyword);
	    } else if (!ustricmp(p->keyword, L"contents")) {
		ret.contents_text = uadv(p->keyword);
	    }
	}
    }

    for (p = source; p; p = p->next) {
	if (p->type == para_Config) {
	    wchar_t *k = p->keyword;
            bool generic = false;

            if (!chm_mode && !ustrnicmp(k, L"html-", 5)) {
                k += 5;
            } else if (!chm_mode && !ustrnicmp(k, L"xhtml-", 6)) {
                k += 6;
            } else if (chm_mode && !ustrnicmp(k, L"chm-", 4)) {
                k += 4;
            } else if (!ustrnicmp(k, L"htmlall-", 8)) {
                k += 8;
                /* In this mode, only accept directives that don't
                 * vary completely between the HTML and CHM output
                 * types. */
                generic = true;
            } else {
                continue;
            }

	    if (!ustricmp(k, L"restrict-charset")) {
		ret.restrict_charset = charset_from_ustr(
                    &p->fpos, uadv(k), es);
	    } else if (!ustricmp(k, L"output-charset")) {
		ret.output_charset = charset_from_ustr(
                    &p->fpos, uadv(k), es);
	    } else if (!ustricmp(k, L"version")) {
		wchar_t *vername = uadv(k);
		static const struct {
		    const wchar_t *name;
		    int ver;
		} versions[] = {
		    {L"html3.2", HTML_3_2},
		    {L"html4", HTML_4},
		    {L"iso-html", ISO_HTML},
		    {L"xhtml1.0transitional", XHTML_1_0_TRANSITIONAL},
		    {L"xhtml1.0strict", XHTML_1_0_STRICT}
		};
		int i;

		for (i = 0; i < (int)lenof(versions); i++)
		    if (!ustricmp(versions[i].name, vername))
			break;

		if (i == lenof(versions))
		    err_htmlver(es, &p->fpos, vername);
		else
		    ret.htmlver = versions[i].ver;
	    } else if (!ustricmp(k, L"single-filename")) {
		sfree(ret.single_filename);
		ret.single_filename = dupstr(adv(p->origkeyword));
	    } else if (!ustricmp(k, L"contents-filename")) {
		sfree(ret.contents_filename);
		ret.contents_filename = dupstr(adv(p->origkeyword));
	    } else if (!ustricmp(k, L"index-filename")) {
		sfree(ret.index_filename);
		ret.index_filename = dupstr(adv(p->origkeyword));
	    } else if (!ustricmp(k, L"template-filename")) {
		sfree(ret.template_filename);
		ret.template_filename = dupstr(adv(p->origkeyword));
	    } else if (!ustricmp(k, L"template-fragment")) {
		char *frag = adv(p->origkeyword);
		if (*frag) {
		    while (ret.ntfragments--)
			sfree(ret.template_fragments[ret.ntfragments]);
		    sfree(ret.template_fragments);
		    ret.template_fragments = NULL;
		    ret.ntfragments = 0;
		    while (*frag) {
			ret.ntfragments++;
			ret.template_fragments =
			    sresize(ret.template_fragments,
				    ret.ntfragments, char *);
			ret.template_fragments[ret.ntfragments-1] =
			    dupstr(frag);
			frag = adv(frag);
		    }
		} else
		    err_cfginsufarg(es, &p->fpos, p->origkeyword, 1);
	    } else if (!ustricmp(k, L"chapter-numeric")) {
		ret.achapter.just_numbers = utob(uadv(k));
	    } else if (!ustricmp(k, L"chapter-shownumber")) {
		ret.achapter.number_at_all = utob(uadv(k));
	    } else if (!ustricmp(k, L"suppress-navlinks")) {
		ret.navlinks = !utob(uadv(k));
	    } else if (!ustricmp(k, L"rellinks")) {
		ret.rellinks = utob(uadv(k));
	    } else if (!ustricmp(k, L"chapter-suffix")) {
		ret.achapter.number_suffix = uadv(k);
	    } else if (!ustricmp(k, L"leaf-level")) {
		wchar_t *u = uadv(k);
		if (!ustricmp(u, L"infinite") ||
		    !ustricmp(u, L"infinity") ||
		    !ustricmp(u, L"inf"))
		    ret.leaf_level = -1;   /* represents infinity */
		else
		    ret.leaf_level = utoi(u);
	    } else if (!ustricmp(k, L"section-numeric")) {
		wchar_t *q = uadv(k);
		int n = 0;
		if (uisdigit(*q)) {
		    n = utoi(q);
		    q = uadv(q);
		}
		if (n >= ret.nasect) {
		    int i;
		    ret.asect = sresize(ret.asect, n+1, sectlevel);
		    for (i = ret.nasect; i <= n; i++)
			ret.asect[i] = ret.asect[ret.nasect-1];
		    ret.nasect = n+1;
		}
		ret.asect[n].just_numbers = utob(q);
	    } else if (!ustricmp(k, L"section-shownumber")) {
		wchar_t *q = uadv(k);
		int n = 0;
		if (uisdigit(*q)) {
		    n = utoi(q);
		    q = uadv(q);
		}
		if (n >= ret.nasect) {
		    int i;
		    ret.asect = sresize(ret.asect, n+1, sectlevel);
		    for (i = ret.nasect; i <= n; i++)
			ret.asect[i] = ret.asect[ret.nasect-1];
		    ret.nasect = n+1;
		}
		ret.asect[n].number_at_all = utob(q);
	    } else if (!ustricmp(k, L"section-suffix")) {
		wchar_t *q = uadv(k);
		int n = 0;
		if (uisdigit(*q)) {
		    n = utoi(q);
		    q = uadv(q);
		}
		if (n >= ret.nasect) {
		    int i;
		    ret.asect = sresize(ret.asect, n+1, sectlevel);
		    for (i = ret.nasect; i <= n; i++) {
			ret.asect[i] = ret.asect[ret.nasect-1];
		    }
		    ret.nasect = n+1;
		}
		ret.asect[n].number_suffix = q;
	    } else if (!ustricmp(k, L"contents-depth") ||
		       !ustrnicmp(k, L"contents-depth-", 15)) {
		/*
		 * Relic of old implementation: this directive used
		 * to be written as \cfg{html-contents-depth-3}{2}
		 * rather than the usual Halibut convention of
		 * \cfg{html-contents-depth}{3}{2}. We therefore
		 * support both.
		 */
		wchar_t *q = k[14] ? k+15 : uadv(k);
		int n = 0;
		if (uisdigit(*q)) {
		    n = utoi(q);
		    q = uadv(q);
		}
		if (n >= ret.ncdepths) {
		    int i;
		    ret.contents_depths =
			sresize(ret.contents_depths, n+1, int);
		    for (i = ret.ncdepths; i <= n; i++) {
			ret.contents_depths[i] = i+2;
		    }
		    ret.ncdepths = n+1;
		}
		ret.contents_depths[n] = utoi(q);
	    } else if (!ustricmp(k, L"head-end")) {
		ret.head_end = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"body-tag")) {
		ret.body_tag = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"body-start")) {
		ret.body_start = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"body-end")) {
		ret.body_end = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"address-start")) {
		ret.addr_start = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"address-end")) {
		ret.addr_end = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"navigation-attributes")) {
		ret.nav_attr = adv(p->origkeyword);
	    } else if (!ustricmp(k, L"author")) {
		ret.author = uadv(k);
	    } else if (!ustricmp(k, L"description")) {
		ret.description = uadv(k);
	    } else if (!ustricmp(k, L"suppress-address")) {
		ret.address_section = !utob(uadv(k));
	    } else if (!ustricmp(k, L"versionid")) {
		ret.visible_version_id = utob(uadv(k));
	    } else if (!ustricmp(k, L"quotes")) {
		if (*uadv(k) && *uadv(uadv(k))) {
		    ret.lquote = uadv(k);
		    ret.rquote = uadv(ret.lquote);
		}
	    } else if (!ustricmp(k, L"leaf-contains-contents")) {
		ret.leaf_contains_contents = utob(uadv(k));
	    } else if (!ustricmp(k, L"leaf-smallest-contents")) {
		ret.leaf_smallest_contents = utoi(uadv(k));
	    } else if (!ustricmp(k, L"index-text")) {
		ret.index_text = uadv(k);
	    } else if (!ustricmp(k, L"contents-text")) {
		ret.contents_text = uadv(k);
	    } else if (!ustricmp(k, L"preamble-text")) {
		ret.preamble_text = uadv(k);
	    } else if (!ustricmp(k, L"title-separator")) {
		ret.title_separator = uadv(k);
	    } else if (!ustricmp(k, L"nav-prev-text")) {
		ret.nav_prev_text = uadv(k);
	    } else if (!ustricmp(k, L"nav-next-text")) {
		ret.nav_next_text = uadv(k);
	    } else if (!ustricmp(k, L"nav-up-text")) {
		ret.nav_up_text = uadv(k);
	    } else if (!ustricmp(k, L"nav-separator")) {
		ret.nav_separator = uadv(k);
	    } else if (!ustricmp(k, L"index-main-separator")) {
		ret.index_main_sep = uadv(k);
	    } else if (!ustricmp(k, L"index-multiple-separator")) {
		ret.index_multi_sep = uadv(k);
	    } else if (!ustricmp(k, L"pre-versionid")) {
		ret.pre_versionid = uadv(k);
	    } else if (!ustricmp(k, L"post-versionid")) {
		ret.post_versionid = uadv(k);
	    } else if (!generic && !ustricmp(
                           k, chm_mode ? L"filename" : L"mshtmlhelp-chm")) {
		sfree(ret.chm_filename);
		ret.chm_filename = dupstr(adv(p->origkeyword));
	    } else if (!generic && !ustricmp(
                           k, chm_mode ? L"contents-name" :
                           L"mshtmlhelp-contents")) {
		sfree(ret.hhc_filename);
		ret.hhc_filename = dupstr(adv(p->origkeyword));
	    } else if (!generic && !ustricmp(
                           k, chm_mode ? L"index-name" :
                           L"mshtmlhelp-index")) {
		sfree(ret.hhk_filename);
		ret.hhk_filename = dupstr(adv(p->origkeyword));
	    } else if (!generic && !chm_mode &&
                       !ustricmp(k, L"mshtmlhelp-project")) {
		sfree(ret.hhp_filename);
		ret.hhp_filename = dupstr(adv(p->origkeyword));
	    } else if (!generic && chm_mode &&
                       !ustricmp(k, L"extra-file")) {
                char *diskname, *chmname;

                diskname = adv(p->origkeyword);
                if (*diskname) {
                    chmname = adv(diskname);
                    if (!*chmname)
                        chmname = diskname;

                    if (chmname[0] == '#' || chmname[0] == '$')
                        err_chm_badname(es, &p->fpos, chmname);

                    if (ret.nchmextrafiles >= ret.chmextrafilesize) {
                        ret.chmextrafilesize = ret.nchmextrafiles * 5 / 4 + 32;
                        ret.chm_extrafiles = sresize(
                            ret.chm_extrafiles, ret.chmextrafilesize, char *);
                        ret.chm_extranames = sresize(
                            ret.chm_extranames, ret.chmextrafilesize, char *);
                    }
                    ret.chm_extrafiles[ret.nchmextrafiles] = dupstr(diskname);
                    ret.chm_extranames[ret.nchmextrafiles] =
                        dupstr(chmname);
                    ret.nchmextrafiles++;
                }
	    }
	}
    }

    if (!chm_mode) {
        /*
         * If we're in HTML mode but using the old-style options to
         * output HTML Help Workshop auxiliary files, do some
         * consistency checking.
         */

        /*
         * Enforce that the CHM and HHP filenames must either be both
         * present or both absent. If one is present but not the other,
         * turn both off.
         */
        if (!ret.chm_filename ^ !ret.hhp_filename) {
            err_chmnames(es);
            sfree(ret.chm_filename); ret.chm_filename = NULL;
            sfree(ret.hhp_filename); ret.hhp_filename = NULL;
        }
        /*
         * And if we're not generating an HHP, there's no need for HHC
         * or HHK.
         */
        if (!ret.hhp_filename) {
            sfree(ret.hhc_filename); ret.hhc_filename = NULL;
            sfree(ret.hhk_filename); ret.hhk_filename = NULL;
        }
    }

    /*
     * Now process fallbacks on quote characters.
     */
    while (*uadv(ret.rquote) && *uadv(uadv(ret.rquote)) &&
	   (!cvt_ok(ret.restrict_charset, ret.lquote) ||
	    !cvt_ok(ret.restrict_charset, ret.rquote))) {
	ret.lquote = uadv(ret.rquote);
	ret.rquote = uadv(ret.lquote);
    }

    return ret;
}

paragraph *html_config_filename(char *filename)
{
    /*
     * If the user passes in a single filename as a parameter to
     * the `--html' command-line option, then we should assume it
     * to imply _two_ config directives:
     * \cfg{html-single-filename}{whatever} and
     * \cfg{html-leaf-level}{0}; the rationale being that the user
     * wants their output _in that file_.
     */
    paragraph *p, *q;

    p = cmdline_cfg_simple("html-single-filename", filename, NULL);
    q = cmdline_cfg_simple("html-leaf-level", "0", NULL);
    p->next = q;
    return p;
}

paragraph *chm_config_filename(char *filename)
{
    return cmdline_cfg_simple("chm-filename", filename, NULL);
}

static void html_backend_common(paragraph *sourceform, keywordlist *keywords,
                                indexdata *idx, errorstate *es, bool chm_mode)
{
    paragraph *p;
    htmlsect *topsect;
    htmlconfig conf;
    htmlfilelist files = { NULL, NULL, NULL, NULL, NULL, NULL };
    htmlsectlist sects = { NULL, NULL }, nonsects = { NULL, NULL };
    struct chm *chm = NULL;
    bool has_index, hhk_needed = false;

    conf = html_configure(sourceform, chm_mode, es);

    /*
     * We're going to make heavy use of paragraphs' private data
     * fields in the forthcoming code. Clear them first, so we can
     * reliably tell whether we have auxiliary data for a
     * particular paragraph.
     */
    for (p = sourceform; p; p = p->next)
	p->private_data = NULL;

    files.frags = newtree234(html_fragment_compare, NULL);
    files.files = newtree234(html_filename_compare, NULL);

    /*
     * Start by figuring out into which file each piece of the
     * document should be put. We'll do this by inventing an
     * `htmlsect' structure and stashing it in the private_data
     * field of each section paragraph; we also need one additional
     * htmlsect for the document index, which won't show up in the
     * source form but needs to be consistently mentioned in
     * contents links.
     * 
     * While we're here, we'll also invent the HTML fragment name(s)
     * for each section.
     */
    {
	htmlsect *sect;
	int d;

	topsect = html_new_sect(&sects, NULL, &conf);
	topsect->type = TOP;
	topsect->title = NULL;
	topsect->text = sourceform;
	topsect->contents_depth = contents_depth(conf, 0);
	html_file_section(&conf, &files, topsect, -1);

	for (p = sourceform; p; p = p->next)
	    if (is_heading_type(p->type)) {
		d = heading_depth(p);

		if (p->type == para_Title) {
		    topsect->title = p;
		    continue;
		}

		sect = html_new_sect(&sects, p, &conf);
		sect->text = p->next;

		sect->contents_depth = contents_depth(conf, d+1) - (d+1);

		if (p->parent) {
		    sect->parent = (htmlsect *)p->parent->private_data;
		    assert(sect->parent != NULL);
		} else
		    sect->parent = topsect;
		p->private_data = sect;

		html_file_section(&conf, &files, sect, d);

		{
		    int i;
		    for (i=0; i < conf.ntfragments; i++) {
			sect->fragments[i] =
			    html_format(p, conf.template_fragments[i]);
			sect->fragments[i] =
			    html_sanitise_fragment(&files, sect->file,
						   sect->fragments[i]);
		    }
		}
	    }

	/*
	 * And the index, if we have one. Note that we don't output
	 * an index as an HTML file if we're outputting one as a
	 * .HHK (in either of the HTML or CHM output modes).
	 */
	has_index = (count234(idx->entries) > 0);
	if (has_index && !chm_mode && !conf.hhk_filename) {
	    sect = html_new_sect(&sects, NULL, &conf);
	    sect->text = NULL;
	    sect->type = INDEX;
	    sect->parent = topsect;
            sect->contents_depth = 0;
	    html_file_section(&conf, &files, sect, 0);   /* peer of chapters */
	    sect->fragments[0] = utoa_dup(conf.index_text, CS_ASCII);
	    sect->fragments[0] = html_sanitise_fragment(&files, sect->file,
							sect->fragments[0]);
	    files.index = sect->file;
	}
    }

    /*
     * Go through the keyword list and sort out fragment IDs for
     * all the potentially referenced paragraphs which _aren't_
     * headings.
     */
    {
	int i;
	keyword *kw;
	htmlsect *sect;

	for (i = 0; (kw = index234(keywords->keys, i)) != NULL; i++) {
	    paragraph *q, *p = kw->para;

	    if (!is_heading_type(p->type)) {
		htmlsect *parent;

		/*
		 * Find the paragraph's parent htmlsect, to
		 * determine which file it will end up in.
		 */
		q = p->parent;
		if (!q) {
		    /*
		     * Preamble paragraphs have no parent. So if we
		     * have a non-heading with no parent, it must
		     * be preamble, and therefore its parent
		     * htmlsect must be the preamble one.
		     */
		    assert(sects.head &&
			   sects.head->type == TOP);
		    parent = sects.head;
		} else
		    parent = (htmlsect *)q->private_data;

		/*
		 * Now we can construct an htmlsect for this
		 * paragraph itself, taking care to put it in the
		 * list of non-sections rather than the list of
		 * sections (so that traverses of the `sects' list
		 * won't attempt to add it to the contents or
		 * anything weird like that).
		 */
		sect = html_new_sect(&nonsects, p, &conf);
		sect->file = parent->file;
		sect->parent = parent;
		p->private_data = sect;

		/*
		 * Fragment IDs for these paragraphs will simply be
		 * `p' followed by an integer.
		 */
		sect->fragments[0] = snewn(40, char);
		sprintf(sect->fragments[0], "p%d",
			sect->file->last_fragment_number++);
		sect->fragments[0] = html_sanitise_fragment(&files, sect->file,
							    sect->fragments[0]);
	    }
	}
    }

    /*
     * Reset the fragment numbers in each file. I've just used them
     * to generate `p' fragment IDs for non-section paragraphs
     * (numbered list elements, bibliocited), and now I want to use
     * them for `i' fragment IDs for index entries.
     */
    {
	htmlfile *file;
	for (file = files.head; file; file = file->next)
	    file->last_fragment_number = 0;
    }

    /*
     * Now sort out the index. This involves:
     * 
     * 	- For each index term, we set up an htmlindex structure to
     * 	  store all the references to that term.
     * 
     * 	- Then we make a pass over the actual document, finding
     * 	  every word_IndexRef; for each one, we actually figure out
     * 	  the HTML filename/fragment pair we will use to reference
     * 	  it, store that information in the private data field of
     * 	  the word_IndexRef itself (so we can recreate it when the
     * 	  time comes to output our HTML), and add a reference to it
     * 	  to the index term in question.
     */
    {
	int i;
	indexentry *entry;
	htmlsect *lastsect;
	word *w;

	/*
	 * Set up the htmlindex structures.
	 */

	for (i = 0; (entry = index234(idx->entries, i)) != NULL; i++) {
	    htmlindex *hi = snew(htmlindex);

	    hi->nrefs = hi->refsize = 0;
	    hi->refs = NULL;

	    entry->backend_data = hi;
	}

	/*
	 * Run over the document inventing fragments. Each fragment
	 * is of the form `i' followed by an integer.
	 */
	lastsect = sects.head;	       /* this is always the top section */
	for (p = sourceform; p; p = p->next) {
	    if (is_heading_type(p->type) && p->type != para_Title)
		lastsect = (htmlsect *)p->private_data;

	    for (w = p->words; w; w = w->next)
		if (w->type == word_IndexRef) {
		    htmlindexref *hr = snew(htmlindexref);
		    indextag *tag;
		    int i;

		    hr->referenced = hr->generated = false;
		    hr->section = lastsect;
		    {
			char buf[40];
			sprintf(buf, "i%d",
				lastsect->file->last_fragment_number++);
			hr->fragment = dupstr(buf);
			hr->fragment =
			    html_sanitise_fragment(&files, hr->section->file,
						   hr->fragment);
		    }
		    w->private_data = hr;

		    tag = index_findtag(idx, w->text);
		    if (!tag)
			break;

		    for (i = 0; i < tag->nrefs; i++) {
			indexentry *entry = tag->refs[i];
			htmlindex *hi = (htmlindex *)entry->backend_data;

			if (hi->nrefs >= hi->refsize) {
			    hi->refsize += 32;
			    hi->refs = sresize(hi->refs, hi->refsize, word *);
			}

			hi->refs[hi->nrefs++] = w;
		    }
		}
	}
    }

    if (chm_mode)
        chm = chm_new();

    /*
     * Now we're ready to write out the actual HTML files.
     * 
     * For each file:
     * 
     *  - we open that file and write its header
     *  - we run down the list of sections
     * 	- for each section directly contained within that file, we
     * 	  output the section text
     * 	- for each section which is not in the file but which has a
     * 	  parent that is, we output a contents entry for the
     * 	  section if appropriate
     *  - finally, we output the file trailer and close the file.
     */
    {
	htmlfile *f, *prevf;
	htmlsect *s;
	paragraph *p;

	prevf = NULL;

	for (f = files.head; f; f = f->next) {
	    htmloutput ho;
	    bool displaying;
	    enum LISTTYPE { NOLIST, UL, OL, DL };
	    enum ITEMTYPE { NOITEM, LI, DT, DD };
	    struct stackelement {
		struct stackelement *next;
		enum LISTTYPE listtype;
		enum ITEMTYPE itemtype;
	    } *stackhead;

#define listname(lt) ( (lt)==UL ? "ul" : (lt)==OL ? "ol" : "dl" )
#define itemname(lt) ( (lt)==LI ? "li" : (lt)==DT ? "dt" : "dd" )

            if (chm)
                ho_setup_chm(&ho, chm, f->filename);
	    else if (!strcmp(f->filename, "-"))
                ho_setup_stdio(&ho, stdout);
            else
                ho_setup_file(&ho, f->filename);

	    ho.charset = conf.output_charset;
	    ho.restrict_charset = conf.restrict_charset;
	    ho.cstate = charset_init_state;
            ho.es = es;
	    ho.ver = conf.htmlver;
	    ho.state = HO_NEUTRAL;
	    ho.contents_level = 0;
	    ho.hackflags = 0;	       /* none of these thankyouverymuch */
	    ho.hacklimit = -1;

	    /* <!DOCTYPE>. */
	    switch (conf.htmlver) {
	      case HTML_3_2:
                ho_string(&ho, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD "
                          "HTML 3.2 Final//EN\">\n");
		break;
	      case HTML_4:
                ho_string(&ho, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML"
                          " 4.01//EN\"\n\"http://www.w3.org/TR/html4/"
                          "strict.dtd\">\n");
		break;
	      case ISO_HTML:
                ho_string(&ho, "<!DOCTYPE HTML PUBLIC \"ISO/IEC "
                          "15445:2000//DTD HTML//EN\">\n");
		break;
	      case XHTML_1_0_TRANSITIONAL:
                ho_string(&ho, "<?xml version=\"1.0\" encoding=\"");
                ho_string(&ho, charset_to_mimeenc(conf.output_charset));
                ho_string(&ho, "\"?>\n"
                          "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML"
                          " 1.0 Transitional//EN\"\n\"http://www.w3.org/TR/"
                          "xhtml1/DTD/xhtml1-transitional.dtd\">\n");
		break;
	      case XHTML_1_0_STRICT:
                ho_string(&ho, "<?xml version=\"1.0\" encoding=\"");
                ho_string(&ho, charset_to_mimeenc(conf.output_charset));
                ho_string(&ho, "\"?>\n"
                          "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML"
                          " 1.0 Strict//EN\"\n\"http://www.w3.org/TR/xhtml1/"
                          "DTD/xhtml1-strict.dtd\">\n");
		break;
	    }

	    element_open(&ho, "html");
	    if (is_xhtml(conf.htmlver)) {
		element_attr(&ho, "xmlns", "http://www.w3.org/1999/xhtml");
	    }
	    html_nl(&ho);

	    element_open(&ho, "head");
	    html_nl(&ho);

	    element_empty(&ho, "meta");
	    element_attr(&ho, "http-equiv", "content-type");
	    {
		char buf[200];
		sprintf(buf, "text/html; charset=%.150s",
			charset_to_mimeenc(conf.output_charset));
		element_attr(&ho, "content", buf);
	    }
	    html_nl(&ho);

            {
                rdstringc rs = { 0, 0, NULL };
                rdaddsc(&rs, "Halibut, ");
                rdaddsc(&rs, version);
                if (rs.text) {
                    element_empty(&ho, "meta");
                    element_attr(&ho, "name", "generator");
                    element_attr(&ho, "content", rs.text);
                    html_nl(&ho);
                    sfree(rs.text);
                }
	    }

	    if (conf.author) {
		element_empty(&ho, "meta");
		element_attr(&ho, "name", "author");
		element_attr_w(&ho, "content", conf.author);
		html_nl(&ho);
	    }

	    if (conf.description) {
		element_empty(&ho, "meta");
		element_attr(&ho, "name", "description");
		element_attr_w(&ho, "content", conf.description);
		html_nl(&ho);
	    }

	    element_open(&ho, "title");
	    if (f->first && f->first->title) {
		html_words(&ho, f->first->title->words, NOTHING,
			   f, keywords, &conf);

		assert(f->last);
		if (f->last != f->first && f->last->title) {
		    html_text(&ho, conf.title_separator);
		    html_words(&ho, f->last->title->words, NOTHING,
			       f, keywords, &conf);
		}
	    }
	    element_close(&ho, "title");
	    html_nl(&ho);

	    if (conf.rellinks) {

		if (prevf) {
		    element_empty(&ho, "link");
		    element_attr(&ho, "rel", "previous");
		    element_attr(&ho, "href", prevf->filename);
		    html_nl(&ho);
		}

		if (f != files.head) {
		    element_empty(&ho, "link");
		    element_attr(&ho, "rel", "ToC");
		    element_attr(&ho, "href", files.head->filename);
		    html_nl(&ho);
		}

		if (conf.leaf_level > 0) {
		    htmlsect *p = f->first->parent;
		    assert(p == f->last->parent);
		    if (p) {
			element_empty(&ho, "link");
			element_attr(&ho, "rel", "up");
			element_attr(&ho, "href", p->file->filename);
			html_nl(&ho);
		    }
		}

		if (has_index && files.index && f != files.index) {
		    element_empty(&ho, "link");
		    element_attr(&ho, "rel", "index");
		    element_attr(&ho, "href", files.index->filename);
		    html_nl(&ho);
		}

		if (f->next) {
		    element_empty(&ho, "link");
		    element_attr(&ho, "rel", "next");
		    element_attr(&ho, "href", f->next->filename);
		    html_nl(&ho);
		}

	    }

	    if (conf.head_end)
		html_raw(&ho, conf.head_end);

	    /*
	     * Add any <head> data defined in specific sections
	     * that go in this file. (This is mostly to allow <meta
	     * name="AppleTitle"> tags for Mac online help.)
	     */
	    for (s = sects.head; s; s = s->next) {
		if (s->file == f && s->text) {
		    for (p = s->text;
			 p && (p == s->text || p->type == para_Title ||
			       !is_heading_type(p->type));
			 p = p->next) {
			if (p->type == para_Config) {
			    if (!ustricmp(p->keyword, L"html-local-head")) {
				html_raw(&ho, adv(p->origkeyword));
			    }
			}
		    }
		}
	    }

	    element_close(&ho, "head");
	    html_nl(&ho);

	    if (conf.body_tag)
		html_raw(&ho, conf.body_tag);
	    else
		element_open(&ho, "body");
	    html_nl(&ho);

	    if (conf.body_start)
		html_raw(&ho, conf.body_start);

	    /*
	     * Write out a nav bar. Special case: we don't do this
	     * if there is only one file.
	     */
	    if (conf.navlinks && files.head != files.tail) {
		element_open(&ho, "p");
		if (conf.nav_attr)
		    html_raw_as_attr(&ho, conf.nav_attr);

		if (prevf) {
		    element_open(&ho, "a");
		    element_attr(&ho, "href", prevf->filename);
		}
		html_text(&ho, conf.nav_prev_text);
		if (prevf)
		    element_close(&ho, "a");

		html_text(&ho, conf.nav_separator);

		if (f != files.head) {
		    element_open(&ho, "a");
		    element_attr(&ho, "href", files.head->filename);
		}
		html_text(&ho, conf.contents_text);
		if (f != files.head)
		    element_close(&ho, "a");

		/* We don't bother with "Up" links for leaf-level 1,
		 * as they would be identical to the "Contents" links. */
		if (conf.leaf_level >= 2) {
		    htmlsect *p = f->first->parent;
		    assert(p == f->last->parent);
		    html_text(&ho, conf.nav_separator);
		    if (p) {
			element_open(&ho, "a");
			element_attr(&ho, "href", p->file->filename);
		    }
		    html_text(&ho, conf.nav_up_text);
		    if (p) {
			element_close(&ho, "a");
		    }
		}

		if (has_index && files.index) {
		    html_text(&ho, conf.nav_separator);
		    if (f != files.index) {
			element_open(&ho, "a");
			element_attr(&ho, "href", files.index->filename);
		    }
		    html_text(&ho, conf.index_text);
		    if (f != files.index)
			element_close(&ho, "a");
		}

		html_text(&ho, conf.nav_separator);

		if (f->next) {
		    element_open(&ho, "a");
		    element_attr(&ho, "href", f->next->filename);
		}
		html_text(&ho, conf.nav_next_text);
		if (f->next)
		    element_close(&ho, "a");

		element_close(&ho, "p");
		html_nl(&ho);
	    }
	    prevf = f;

	    /*
	     * Special case: for single-file mode, we output the top
	     * section title before the TOC.
	     */
	    if (files.head == files.tail && sects.head->type == TOP) {
		element_open(&ho, "h1");

		/*
		 * Provide anchor(s) for cross-links to target.
		 */
		{
		    int i;
		    for (i=0; i < conf.ntfragments; i++)
			if (sects.head->fragments[i])
			    html_fragment(&ho, sects.head->fragments[i]);
		}

		html_section_title(&ho, sects.head, f, keywords, &conf, true);

		element_close(&ho, "h1");
	    }

	    /*
	     * Write out a prefix TOC for the file (if a leaf file).
	     * 
	     * We start by going through the section list and
	     * collecting the sections which need to be added to
	     * the contents. On the way, we also test to see if
	     * this file is a leaf file (defined as one which
	     * contains all descendants of any section it
	     * contains), because this will play a part in our
	     * decision on whether or not to _output_ the TOC.
	     */
	    {
		int ntoc = 0, tocsize = 0, tocstartidx = 0;
		htmlsect **toc = NULL;
		bool leaf = true;

		for (s = sects.head; s; s = s->next) {
		    htmlsect *a, *ac;
		    int depth, adepth;

		    /*
		     * Search up from this section until we find
		     * the highest-level one which belongs in this
		     * file.
		     */
		    depth = adepth = 0;
		    a = NULL;
		    for (ac = s; ac; ac = ac->parent) {
			if (ac->file == f) {
			    a = ac;
			    adepth = depth;
			}
			depth++;
		    }

		    if (s->file != f && a != NULL)
			leaf = false;

		    if (a) {
			if (adepth <= a->contents_depth) {
			    if (ntoc >= tocsize) {
				tocsize += 64;
				toc = sresize(toc, tocsize, htmlsect *);
			    }
			    toc[ntoc++] = s;
			}
		    }
		}

		/*
		 * Special case: for single-file mode, we don't output
		 * the first section TOC entry if it's the top since we
		 * have already output a section title for it above the
		 * TOC, and since we don't output the top TOC entry, we
		 * reduce the level of the remaining TOC entries by one
		 * so that they are output correctly one level up from
		 * where they would have been.
		 */
		tocstartidx = (files.head == files.tail && ntoc > 0 &&
			       toc[0]->type == TOP) ? 1 : 0;
		if (leaf && conf.leaf_contains_contents &&
		    ntoc >= conf.leaf_smallest_contents &&
		    tocstartidx < ntoc) {
		    int i;

		    for (i = tocstartidx; i < ntoc; i++) {
			htmlsect *s = toc[i];
			int hlevel = (s->type == TOP ? -1 :
				      s->type == INDEX ? 0 :
				      heading_depth(s->title))
			    - tocstartidx - f->min_heading_depth + 1;

			assert(hlevel >= 1);
			html_contents_entry(&ho, hlevel, s,
					    f, keywords, &conf);
		    }
		    html_contents_entry(&ho, 0, NULL, f, keywords, &conf);
		}
	    }

	    /*
	     * Now go through the document and output some real
	     * text.
	     */
	    displaying = false;
	    for (s = sects.head; s; s = s->next) {
		if (s->file == f) {
		    /*
		     * This section belongs in this file.
		     * Display it.
		     */
		    displaying = true;
		} else {
		    /*
		     * Doesn't belong in this file, but it may be
		     * a descendant of a section which does, in
		     * which case we should consider it for the
		     * main TOC of this file (for non-leaf files).
		     */
		    htmlsect *a, *ac;
		    int depth, adepth;

		    displaying = false;

		    /*
		     * Search up from this section until we find
		     * the highest-level one which belongs in this
		     * file.
		     */
		    depth = adepth = 0;
		    a = NULL;
		    for (ac = s; ac; ac = ac->parent) {
			if (ac->file == f) {
			    a = ac;
			    adepth = depth;
			}
			depth++;
		    }

		    if (a != NULL) {
			/*
			 * This section does not belong in this
			 * file, but an ancestor of it does. Write
			 * out a contents table entry, if the depth
			 * doesn't exceed the maximum contents
			 * depth for the ancestor section.
			 */
			if (adepth <= a->contents_depth) {
			    html_contents_entry(&ho, adepth, s,
						f, keywords, &conf);
			}
		    }
		}

		if (displaying) {
		    int hlevel;
		    char htag[3];

		    html_contents_entry(&ho, 0, NULL, f, keywords, &conf);

		    /*
		     * Display the section heading.
		     *
		     * Special case: for single-file mode, we don't
		     * output the top section title since we have
		     * already output it before the TOC.
		     */
		    if (files.head != files.tail || s->type != TOP) {
			hlevel = (s->type == TOP ? -1 :
				  s->type == INDEX ? 0 :
				  heading_depth(s->title))
			    - f->min_heading_depth + 1;
			assert(hlevel >= 1);
			/* HTML headings only go up to <h6> */
			if (hlevel > 6)
			    hlevel = 6;
			htag[0] = 'h';
			htag[1] = '0' + hlevel;
			htag[2] = '\0';
			element_open(&ho, htag);

			/*
			 * Provide anchor(s) for cross-links to target.
			 * 
			 * (Also we'll have to do this separately in
			 * other paragraph types - NumberedList and
			 * BiblioCited.)
			 */
			{
			    int i;
			    for (i=0; i < conf.ntfragments; i++)
				if (s->fragments[i])
				    html_fragment(&ho, s->fragments[i]);
			}

			html_section_title(&ho, s, f, keywords, &conf, true);

			element_close(&ho, htag);
		    }

		    /*
		     * Now display the section text.
		     */
		    if (s->text) {
			stackhead = snew(struct stackelement);
			stackhead->next = NULL;
			stackhead->listtype = NOLIST;
			stackhead->itemtype = NOITEM;

			for (p = s->text;; p = p->next) {
			    enum LISTTYPE listtype;
			    struct stackelement *se;

			    /*
			     * Preliminary switch to figure out what
			     * sort of list we expect to be inside at
			     * this stage.
			     *
			     * Since p may still be NULL at this point,
			     * I invent a harmless paragraph type for
			     * it if it is.
			     */
			    switch (p ? p->type : para_Normal) {
			      case para_Rule:
			      case para_Normal:
			      case para_Copyright:
			      case para_BiblioCited:
			      case para_Code:
			      case para_QuotePush:
			      case para_QuotePop:
			      case para_Chapter:
			      case para_Appendix:
			      case para_UnnumberedChapter:
			      case para_Heading:
			      case para_Subsect:
			      case para_LcontPop:
				listtype = NOLIST;
				break;

			      case para_Bullet:
				listtype = UL;
				break;

			      case para_NumberedList:
				listtype = OL;
				break;

			      case para_DescribedThing:
			      case para_Description:
				listtype = DL;
				break;

			      case para_LcontPush:
				se = snew(struct stackelement);
				se->next = stackhead;
				se->listtype = NOLIST;
				se->itemtype = NOITEM;
				stackhead = se;
				continue;

			      default:     /* some totally non-printing para */
				continue;
			    }

			    html_nl(&ho);

			    /*
			     * Terminate the most recent list item, if
			     * any. (We left this until after
			     * processing LcontPush, since in that case
			     * the list item won't want to be
			     * terminated until after the corresponding
			     * LcontPop.)
			     */
			    if (stackhead->itemtype != NOITEM) {
				element_close(&ho, itemname(stackhead->itemtype));
				html_nl(&ho);
			    }
			    stackhead->itemtype = NOITEM;

			    /*
			     * Terminate the current list, if it's not
			     * the one we want to be in.
			     */
			    if (listtype != stackhead->listtype &&
				stackhead->listtype != NOLIST) {
				element_close(&ho, listname(stackhead->listtype));
				html_nl(&ho);
			    }

			    /*
			     * Leave the loop if our time has come.
			     */
			    if (!p || (is_heading_type(p->type) &&
				       p->type != para_Title))
				break;     /* end of section text */

			    /*
			     * Start a fresh list if necessary.
			     */
			    if (listtype != stackhead->listtype &&
				listtype != NOLIST)
				element_open(&ho, listname(listtype));

			    stackhead->listtype = listtype;

			    switch (p->type) {
			      case para_Rule:
				element_empty(&ho, "hr");
				break;
			      case para_Code:
				html_codepara(&ho, p->words);
				break;
			      case para_Normal:
			      case para_Copyright:
				element_open(&ho, "p");
				html_nl(&ho);
				html_words(&ho, p->words, ALL,
					   f, keywords, &conf);
				html_nl(&ho);
				element_close(&ho, "p");
				break;
			      case para_BiblioCited:
				element_open(&ho, "p");
				if (p->private_data) {
				    htmlsect *s = (htmlsect *)p->private_data;
				    int i;
				    for (i=0; i < conf.ntfragments; i++)
					if (s->fragments[i])
					    html_fragment(&ho, s->fragments[i]);
				}
				html_nl(&ho);
				html_words(&ho, p->kwtext, ALL,
					   f, keywords, &conf);
				html_text(&ho, L" ");
				html_words(&ho, p->words, ALL,
					   f, keywords, &conf);
				html_nl(&ho);
				element_close(&ho, "p");
				break;
			      case para_Bullet:
			      case para_NumberedList:
				element_open(&ho, "li");
				if (p->private_data) {
				    htmlsect *s = (htmlsect *)p->private_data;
				    int i;
				    for (i=0; i < conf.ntfragments; i++)
					if (s->fragments[i])
					    html_fragment(&ho, s->fragments[i]);
				}
				html_nl(&ho);
				stackhead->itemtype = LI;
				html_words(&ho, p->words, ALL,
					   f, keywords, &conf);
				break;
			      case para_DescribedThing:
				element_open(&ho, "dt");
				html_nl(&ho);
				stackhead->itemtype = DT;
				html_words(&ho, p->words, ALL,
					   f, keywords, &conf);
				break;
			      case para_Description:
				element_open(&ho, "dd");
				html_nl(&ho);
				stackhead->itemtype = DD;
				html_words(&ho, p->words, ALL,
					   f, keywords, &conf);
				break;

			      case para_QuotePush:
				element_open(&ho, "blockquote");
				break;
			      case para_QuotePop:
				element_close(&ho, "blockquote");
				break;

			      case para_LcontPop:
				se = stackhead;
				stackhead = stackhead->next;
				assert(stackhead);
				sfree(se);
				break;
			    }
			}

			assert(stackhead && !stackhead->next);
			sfree(stackhead);
		    }
		    
		    if (s->type == INDEX) {
			indexentry *entry;
			int i;

			/*
			 * This section is the index. I'll just
			 * render it as a single paragraph, with a
			 * colon between the index term and the
			 * references, and <br> in between each
			 * entry.
			 */
			element_open(&ho, "p");

			for (i = 0; (entry =
				     index234(idx->entries, i)) != NULL; i++) {
			    htmlindex *hi = (htmlindex *)entry->backend_data;
			    int j;

			    if (i > 0)
				element_empty(&ho, "br");
			    html_nl(&ho);

			    html_words(&ho, entry->text, MARKUP|LINKS,
				       f, keywords, &conf);

			    html_text(&ho, conf.index_main_sep);

			    for (j = 0; j < hi->nrefs; j++) {
				htmlindexref *hr =
				    (htmlindexref *)hi->refs[j]->private_data;
				paragraph *p = hr->section->title;

				if (j > 0)
				    html_text(&ho, conf.index_multi_sep);

				html_href(&ho, f, hr->section->file,
					  hr->fragment);
				hr->referenced = true;
				if (p && p->kwtext)
				    html_words(&ho, p->kwtext, MARKUP|LINKS,
					       f, keywords, &conf);
				else if (p && p->words)
				    html_words(&ho, p->words, MARKUP|LINKS,
					       f, keywords, &conf);
				else {
				    /*
				     * If there is no title at all,
				     * this must be because our
				     * target section is the
				     * preamble section and there
				     * is no title. So we use the
				     * preamble_text.
				     */
				    html_text(&ho, conf.preamble_text);
				}
				element_close(&ho, "a");
			    }
			}
			element_close(&ho, "p");
		    }
		}
	    }

	    html_contents_entry(&ho, 0, NULL, f, keywords, &conf);
	    html_nl(&ho);

	    {
		/*
		 * Footer.
		 */
		bool done_version_ids = false;

		if (conf.address_section)
		    element_empty(&ho, "hr");

		if (conf.body_end)
		    html_raw(&ho, conf.body_end);

		if (conf.address_section) {
		    bool started = false;
		    if (conf.htmlver == ISO_HTML) {
			/*
			 * The ISO-HTML validator complains if
			 * there isn't a <div> tag surrounding the
			 * <address> tag. I'm uncertain of why this
			 * should be - there appears to be no
			 * mention of this in the ISO-HTML spec,
			 * suggesting that it doesn't represent a
			 * change from HTML 4, but nonetheless the
			 * HTML 4 validator doesn't seem to mind.
			 */
			element_open(&ho, "div");
		    }
		    element_open(&ho, "address");
		    if (conf.addr_start) {
			html_raw(&ho, conf.addr_start);
			html_nl(&ho);
			started = true;
		    }
		    if (conf.visible_version_id) {
			for (p = sourceform; p; p = p->next)
			    if (p->type == para_VersionID) {
				if (started)
				    element_empty(&ho, "br");
				html_nl(&ho);
				html_text(&ho, conf.pre_versionid);
				html_words(&ho, p->words, NOTHING,
					   f, keywords, &conf);
				html_text(&ho, conf.post_versionid);
				started = true;
			    }
			done_version_ids = true;
		    }
		    if (conf.addr_end) {
			if (started)
			    element_empty(&ho, "br");
			html_raw(&ho, conf.addr_end);
		    }
		    element_close(&ho, "address");
		    if (conf.htmlver == ISO_HTML)
			element_close(&ho, "div");
		}

		if (!done_version_ids) {
		    /*
		     * If the user didn't want the version IDs
		     * visible, I think we still have a duty to put
		     * them in an HTML comment.
		     */
		    bool started = false;
		    for (p = sourceform; p; p = p->next)
			if (p->type == para_VersionID) {
			    if (!started) {
				html_raw(&ho, "<!-- version IDs:\n");
				started = true;
			    }
			    html_words(&ho, p->words, NOTHING,
				       f, keywords, &conf);
			    html_nl(&ho);
			}
		    if (started)
			html_raw(&ho, "-->\n");
		}
	    }

	    element_close(&ho, "body");
	    html_nl(&ho);
	    element_close(&ho, "html");
	    html_nl(&ho);
	    cleanup(&ho);
	}
    }

    /*
     * Before we start outputting the HTML Help files, check
     * whether there's even going to _be_ an index file: we omit it
     * if the index contains nothing.
     */
    if (chm_mode || conf.hhk_filename) {
	bool ok = false;
	int i;
	indexentry *entry;

	for (i = 0; (entry = index234(idx->entries, i)) != NULL; i++) {
	    htmlindex *hi = (htmlindex *)entry->backend_data;

	    if (hi->nrefs > 0) {
		ok = true;	       /* found an index entry */
		break;
	    }
	}

	if (ok)
	    hhk_needed = true;
    }

    /*
     * If we're doing direct CHM output, tell winchm.c all the things
     * it will need to know aside from the various HTML files'
     * contents.
     */
    if (chm) {
        chm_contents_filename(chm, conf.hhc_filename);
        if (has_index)
            chm_index_filename(chm, conf.hhk_filename);
        chm_default_window(chm, "main");

        {
            htmloutput ho;
            rdstringc rs = {0, 0, NULL};

            ho.charset = CS_CP1252; /* as far as I know, CHM is */
            ho.restrict_charset = CS_CP1252; /* hardwired to this charset */
            ho.cstate = charset_init_state;
            ho.es = es;
            ho.ver = HTML_4;	       /* *shrug* */
            ho.state = HO_NEUTRAL;
            ho.contents_level = 0;
            ho.hackflags = HO_HACK_QUOTENOTHING;

            ho_setup_rdstringc(&ho, &rs);

            ho.hacklimit = 255;
            if (topsect->title)
                html_words(&ho, topsect->title->words, NOTHING,
                           NULL, keywords, &conf);

            rdaddc(&rs, '\0');
            chm_title(chm, rs.text);

            chm_default_topic(chm, files.head->filename);

            chm_add_window(chm, "main", rs.text,
                           conf.hhc_filename, conf.hhk_filename,
                           files.head->filename,
                           /* This first magic number is
                            * fsWinProperties, controlling Navigation
                            * Pane options and the like. Constants
                            * HHWIN_PROP_* in htmlhelp.h. */
                           0x62520,
                           /* This second number is fsToolBarFlags,
                            * mainly controlling toolbar buttons.
                            * Constants HHWIN_BUTTON_*. NOTE: there
                            * are two pairs of bits for Next/Previous
                            * buttons: 7/8 (which do nothing useful),
                            * and 21/22 (which work). (Neither of
                            * these are exposed in the HHW UI, but
                            * they work fine in HH.) We use the
                            * latter. */
                           0x70304e);

            sfree(rs.text);
        }

        {
            htmlfile *f;

            for (f = files.head; f; f = f->next)
                f->chmsect = NULL;
            for (f = files.head; f; f = f->next) {
                htmlsect *s = f->first;
                htmloutput ho;
                rdstringc rs = {0, 0, NULL};

                ho.charset = CS_CP1252;
                ho.restrict_charset = CS_CP1252;
                ho.cstate = charset_init_state;
                ho.es = es;
                ho.ver = HTML_4;	       /* *shrug* */
                ho.state = HO_NEUTRAL;
                ho.contents_level = 0;
                ho.hackflags = HO_HACK_QUOTENOTHING;

                ho_setup_rdstringc(&ho, &rs);
                ho.hacklimit = 255;

                if (f->first->title)
                    html_words(&ho, f->first->title->words, NOTHING,
                               NULL, keywords, &conf);
                else if (f->first->type == INDEX)
                    html_text(&ho, conf.index_text);
                rdaddc(&rs, '\0');
                
                while (s && s->file == f)
                    s = s->parent;

                /*
                 * Special case, as below: the TOP file is not
                 * considered to be the parent of everything else.
                 */
                if (s && s->type == TOP)
                    s = NULL;

                f->chmsect = chm_add_section(chm, s ? s->file->chmsect : NULL,
                                             rs.text, f->filename);

                sfree(rs.text);
            }
        }

        {
            int i;

            for (i = 0; i < conf.nchmextrafiles; i++) {
                const char *fname = conf.chm_extrafiles[i];
                FILE *fp;
                long size;
                char *data;

                fp = fopen(fname, "rb");
                if (!fp) {
                    err_cantopen(es, fname);
                    continue;
                }

                fseek(fp, 0, SEEK_END);
                size = ftell(fp);
                rewind(fp);

                data = snewn(size, char);
                size = fread(data, 1, size, fp);
                fclose(fp);

                chm_add_file(chm, conf.chm_extranames[i], data, size);
                sfree(data);
            }
        }
    }

    /*
     * Output the MS HTML Help supporting files, if requested.
     *
     * A good unofficial reference for these is <http://chmspec.nongnu.org/>.
     */
    if (conf.hhp_filename) {
	htmlfile *f;
	htmloutput ho;

	ho.charset = CS_CP1252;	       /* as far as I know, HHP files are */
	ho.restrict_charset = CS_CP1252;   /* hardwired to this charset */
	ho.cstate = charset_init_state;
        ho.es = es;
	ho.ver = HTML_4;	       /* *shrug* */
	ho.state = HO_NEUTRAL;
	ho.contents_level = 0;
	ho.hackflags = HO_HACK_QUOTENOTHING;

        ho_setup_file(&ho, conf.hhp_filename);

	ho_string(&ho,
                  "[OPTIONS]\n"
                  /* Binary TOC required for Next/Previous nav to work */
                  "Binary TOC=Yes\n"
                  "Compatibility=1.1 or later\n"
                  "Compiled file=");
        ho_string(&ho, conf.chm_filename);
	ho_string(&ho, "\n"
                  "Default Window=main\n"
                  "Default topic=");
        ho_string(&ho, files.head->filename);
	ho_string(&ho, "\n"
                  "Display compile progress=Yes\n"
                  "Full-text search=Yes\n"
                  "Title=");

	ho.hacklimit = 255;
        if (topsect->title)
            html_words(&ho, topsect->title->words, NOTHING,
                       NULL, keywords, &conf);

	ho_string(&ho, "\n");

	/*
	 * These two entries don't seem to be remotely necessary
	 * for a successful run of the help _compiler_, but
	 * omitting them causes the GUI Help Workshop to behave
	 * rather strangely if you try to load the help project
	 * into that and edit it.
	 */
	if (conf.hhc_filename) {
            ho_string(&ho, "Contents file=");
            ho_string(&ho, conf.hhc_filename);
            ho_string(&ho, "\n");
        }
	if (hhk_needed) {
            ho_string(&ho, "Index file=");
            ho_string(&ho, conf.hhk_filename);
            ho_string(&ho, "\n");
        }

        ho_string(&ho, "\n[WINDOWS]\nmain=\"");

	ho.hackflags |= HO_HACK_OMITQUOTES;
	ho.hacklimit = 255;
	html_words(&ho, topsect->title->words, NOTHING,
		   NULL, keywords, &conf);

        ho_string(&ho, "\",\"");
        if (conf.hhc_filename)
            ho_string(&ho, conf.hhc_filename);
        ho_string(&ho, "\",\"");
        if (hhk_needed)
            ho_string(&ho, conf.hhk_filename);
        ho_string(&ho, "\",\"");
        ho_string(&ho, files.head->filename);
        ho_string(&ho, "\",,,,,,"
                  /* This first magic number is fsWinProperties, controlling
                   * Navigation Pane options and the like.
                   * Constants HHWIN_PROP_* in htmlhelp.h. */
                  "0x62520,,"
                  /* This second number is fsToolBarFlags, mainly controlling
                   * toolbar buttons. Constants HHWIN_BUTTON_*.
                   * NOTE: there are two pairs of bits for Next/Previous
                   * buttons: 7/8 (which do nothing useful), and 21/22
                   * (which work). (Neither of these are exposed in the HHW
                   * UI, but they work fine in HH.) We use the latter. */
                  "0x70304e,,,,,,,,0\n");

	/*
	 * The [FILES] section is also not necessary for
	 * compilation (hhc appears to build up a list of needed
	 * files just by following links from the given starting
	 * points), but useful for loading the project into HHW.
	 */
	ho_string(&ho, "\n[FILES]\n");
	for (f = files.head; f; f = f->next) {
	    ho_string(&ho, f->filename);
	    ho_string(&ho, "\n");
        }

        ho_finish(&ho);
    }
    if (chm || conf.hhc_filename) {
	htmlfile *f;
	htmlsect *s, *a;
	htmloutput ho;
	int currdepth = 0;

	ho.charset = CS_CP1252;	       /* as far as I know, HHC files are */
	ho.restrict_charset = CS_CP1252;   /* hardwired to this charset */
	ho.cstate = charset_init_state;
        ho.es = es;
	ho.ver = HTML_4;	       /* *shrug* */
	ho.state = HO_NEUTRAL;
	ho.contents_level = 0;
	ho.hackflags = HO_HACK_QUOTEQUOTES;

        if (chm)
            ho_setup_chm(&ho, chm, conf.hhc_filename);
        else
            ho_setup_file(&ho, conf.hhc_filename);

	/*
	 * Magic DOCTYPE which seems to work for .HHC files. I'm
	 * wary of trying to change it!
	 */
	ho_string(&ho, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML//EN\">\n"
                  "<HTML><HEAD>\n"
                  "<META HTTP-EQUIV=\"Content-Type\" "
                  "CONTENT=\"text/html; charset=");
        ho_string(&ho, charset_to_mimeenc(conf.output_charset));
        ho_string(&ho, "\">\n"
                  "</HEAD><BODY><UL>\n");

	for (f = files.head; f; f = f->next) {
	    /*
	     * For each HTML file, write out a contents entry.
	     */
	    int depth;
            bool leaf = true;

	    /*
	     * Determine the depth of this file in the contents
	     * tree.
	     * 
	     * If the file contains no sections, it is assumed to
	     * have depth zero.
	     */
	    depth = 0;
	    if (f->first)
		for (a = f->first->parent; a && a->type != TOP; a = a->parent)
		    depth++;

	    /*
	     * Determine if this file is a leaf file, by
	     * trawling the section list to see if there's any
	     * section with an ancestor in this file but which
	     * is not itself in this file.
	     *
	     * Special case: for contents purposes, the TOP
	     * file is not considered to be the parent of the
	     * chapter files, so it's always a leaf.
	     * 
	     * A file with no sections in it is also a leaf.
	     */
	    if (f->first && f->first->type != TOP) {
		for (s = f->first; s; s = s->next) {
		    htmlsect *a;

		    if (leaf && s->file != f) {
			for (a = s; a; a = a->parent)
			    if (a->file == f) {
				leaf = false;
				break;
			    }
		    }
		}
	    }

	    /*
	     * Now write out our contents entry.
	     */
	    while (currdepth < depth) {
		ho_string(&ho, "<UL>\n");
		currdepth++;
	    }
	    while (currdepth > depth) {
		ho_string(&ho, "</UL>\n");
		currdepth--;
	    }
            ho_string(&ho, "<LI><OBJECT TYPE=\"text/sitemap\">"
                      "<PARAM NAME=\"Name\" VALUE=\"");
	    ho.hacklimit = 255;
	    if (f->first->title)
		html_words(&ho, f->first->title->words, NOTHING,
			   NULL, keywords, &conf);
	    else if (f->first->type == INDEX)
		html_text(&ho, conf.index_text);
            ho_string(&ho, "\"><PARAM NAME=\"Local\" VALUE=\"");
            ho_string(&ho, f->filename);
            ho_string(&ho, "\"><PARAM NAME=\"ImageNumber\" VALUE=\"");
            ho_string(&ho, leaf ? "11" : "1");
            ho_string(&ho, "\"></OBJECT>\n");
	}

	while (currdepth > 0) {
	    ho_string(&ho, "</UL>\n");
	    currdepth--;
	}

	ho_string(&ho, "</UL></BODY></HTML>\n");

	cleanup(&ho);
    }
    if (hhk_needed) {
	htmlfile *f;
	htmloutput ho;
	indexentry *entry;
	int i;

	/*
	 * First make a pass over all HTML files and set their
	 * `temp' fields to zero, because we're about to use them.
	 */
	for (f = files.head; f; f = f->next)
	    f->temp = 0;

	ho.charset = CS_CP1252;	       /* as far as I know, HHK files are */
	ho.restrict_charset = CS_CP1252;   /* hardwired to this charset */
	ho.cstate = charset_init_state;
        ho.es = es;
	ho.ver = HTML_4;	       /* *shrug* */
	ho.state = HO_NEUTRAL;
	ho.contents_level = 0;
	ho.hackflags = HO_HACK_QUOTEQUOTES;

        if (chm)
            ho_setup_chm(&ho, chm, conf.hhk_filename);
        else
            ho_setup_file(&ho, conf.hhk_filename);

	/*
	 * Magic DOCTYPE which seems to work for .HHK files. I'm
	 * wary of trying to change it!
	 */
        ho_string(&ho, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML//EN\">\n"
                  "<HTML><HEAD>\n"
                  "<META HTTP-EQUIV=\"Content-Type\" "
                  "CONTENT=\"text/html; charset=");
        ho_string(&ho, charset_to_mimeenc(conf.output_charset));
        ho_string(&ho, "\">\n"
                  "</HEAD><BODY><UL>\n");

	/*
	 * Go through the index terms and output each one.
	 */
	for (i = 0; (entry = index234(idx->entries, i)) != NULL; i++) {
	    htmlindex *hi = (htmlindex *)entry->backend_data;
	    int j;

	    if (hi->nrefs > 0) {
		ho_string(&ho, "<LI><OBJECT TYPE=\"text/sitemap\">\n"
                          "<PARAM NAME=\"Name\" VALUE=\"");
		ho.hacklimit = 255;
		html_words(&ho, entry->text, NOTHING,
			   NULL, keywords, &conf);
		ho_string(&ho, "\">\n");

		for (j = 0; j < hi->nrefs; j++) {
		    htmlindexref *hr =
			(htmlindexref *)hi->refs[j]->private_data;

		    /*
		     * Use the temp field to ensure we don't
		     * reference the same file more than once.
		     */
		    if (!hr->section->file->temp) {
			ho_string(&ho, "<PARAM NAME=\"Local\" VALUE=\"");
                        ho_string(&ho, hr->section->file->filename);
                        ho_string(&ho, "\">\n");
			hr->section->file->temp = 1;
		    }

		    hr->referenced = true;
		}

		ho_string(&ho, "</OBJECT>\n");

		/*
		 * Now go through those files and re-clear the temp
		 * fields ready for the _next_ index term.
		 */
		for (j = 0; j < hi->nrefs; j++) {
		    htmlindexref *hr =
			(htmlindexref *)hi->refs[j]->private_data;
		    hr->section->file->temp = 0;
		}
	    }
	}

	ho_string(&ho, "</UL></BODY></HTML>\n");
	cleanup(&ho);
    }

    if (chm) {
        /*
         * Finalise and write out the CHM file.
         */
        const char *data;
        int len;
        FILE *fp;

        fp = fopen(conf.chm_filename, "wb");
        if (!fp) {
            err_cantopenw(es, conf.chm_filename);
        } else {
            data = chm_build(chm, &len);
            fwrite(data, 1, len, fp);
            fclose(fp);
        }

        chm_free(chm);
    }

    /*
     * Go through and check that no index fragments were referenced
     * without being generated, or indeed vice versa.
     * 
     * (When I actually get round to freeing everything, this can
     * probably be the freeing loop as well.)
     */
    for (p = sourceform; p; p = p->next) {
	word *w;
	for (w = p->words; w; w = w->next)
	    if (w->type == word_IndexRef) {
		htmlindexref *hr = (htmlindexref *)w->private_data;

		assert(hr->referenced == hr->generated);
	    }
    }

    /*
     * Free all the working data.
     */
    {
	htmlfragment *frag;
	while ( (frag = (htmlfragment *)delpos234(files.frags, 0)) != NULL ) {
	    /*
	     * frag->fragment is dynamically allocated, but will be
	     * freed when we process the htmlsect structure which
	     * it is attached to.
	     */
	    sfree(frag);
	}
	freetree234(files.frags);
    }
    /*
     * The strings in files.files are all owned by their containing
     * htmlfile structures, so there's no need to free them here.
     */
    freetree234(files.files);
    {
	htmlsect *sect, *tmp;
	sect = sects.head;
	while (sect) {
	    int i;
	    tmp = sect->next;
	    for (i=0; i < conf.ntfragments; i++)
		sfree(sect->fragments[i]);
	    sfree(sect->fragments);
	    sfree(sect);
	    sect = tmp;
	}
	sect = nonsects.head;
	while (sect) {
	    int i;
	    tmp = sect->next;
	    for (i=0; i < conf.ntfragments; i++)
		sfree(sect->fragments[i]);
	    sfree(sect->fragments);
	    sfree(sect);
	    sect = tmp;
	}
    }
    {
	htmlfile *file, *tmp;
	file = files.head;
	while (file) {
	    tmp = file->next;
	    sfree(file->filename);
	    sfree(file);
	    file = tmp;
	}
    }
    {
	int i;
	indexentry *entry;
	for (i = 0; (entry = index234(idx->entries, i)) != NULL; i++) {
	    htmlindex *hi = (htmlindex *)entry->backend_data;
	    sfree(hi);
	}
    }
    {
	paragraph *p;
	word *w;
	for (p = sourceform; p; p = p->next)
	    for (w = p->words; w; w = w->next)
		if (w->type == word_IndexRef) {
		    htmlindexref *hr = (htmlindexref *)w->private_data;
		    assert(hr != NULL);
		    sfree(hr->fragment);
		    sfree(hr);
		}
    }
    sfree(conf.asect);
    sfree(conf.single_filename);
    sfree(conf.contents_filename);
    sfree(conf.index_filename);
    sfree(conf.template_filename);
    while (conf.ntfragments--)
	sfree(conf.template_fragments[conf.ntfragments]);
    sfree(conf.template_fragments);
    while (conf.nchmextrafiles--) {
	sfree(conf.chm_extrafiles[conf.nchmextrafiles]);
	sfree(conf.chm_extranames[conf.nchmextrafiles]);
    }
    sfree(conf.chm_extrafiles);
}

void html_backend(paragraph *sourceform, keywordlist *keywords,
                  indexdata *idx, void *unused, errorstate *es)
{
    IGNORE(unused);
    html_backend_common(sourceform, keywords, idx, es, false);
}

void chm_backend(paragraph *sourceform, keywordlist *keywords,
                 indexdata *idx, void *unused, errorstate *es)
{
    IGNORE(unused);
    html_backend_common(sourceform, keywords, idx, es, true);
}

static void html_file_section(htmlconfig *cfg, htmlfilelist *files,
			      htmlsect *sect, int depth)
{
    htmlfile *file;
    int ldepth;

    /*
     * `depth' is derived from the heading_depth() macro at the top
     * of this file, which counts title as -1, chapter as 0,
     * heading as 1 and subsection as 2. However, the semantics of
     * cfg->leaf_level are defined to count chapter as 1, heading
     * as 2 etc. So first I increment depth :-(
     */
    ldepth = depth + 1;

    if (cfg->leaf_level == 0) {
	/*
	 * leaf_level==0 is a special case, in which everything is
	 * put into a single file.
	 */
	if (!files->single)
	    files->single = html_new_file(files, cfg->single_filename);

	file = files->single;
    } else {
	/*
	 * If the depth of this section is at or above leaf_level,
	 * we invent a fresh file and put this section at its head.
	 * Otherwise, we put it in the same file as its parent
	 * section.
	 * 
	 * Another special value of cfg->leaf_level is -1, which
	 * means infinity (i.e. it's considered to always be
	 * greater than depth).
	 */
	if (cfg->leaf_level > 0 && ldepth > cfg->leaf_level) {
	    /*
	     * We know that sect->parent cannot be NULL. The only
	     * circumstance in which it can be is if sect is at
	     * chapter or appendix level, i.e. ldepth==1; and if
	     * that's the case, then we cannot have entered this
	     * branch unless cfg->leaf_level==0, in which case we
	     * would be in the single-file case above and not here
	     * at all.
	     */
	    assert(sect->parent);

	    file = sect->parent->file;
	} else {
	    if (sect->type == TOP) {
		file = html_new_file(files, cfg->contents_filename);
	    } else if (sect->type == INDEX) {
		file = html_new_file(files, cfg->index_filename);
	    } else {
		char *title;

		assert(ldepth > 0 && sect->title);
		title = html_format(sect->title, cfg->template_filename);
		file = html_new_file(files, title);
		sfree(title);
	    }
	}
    }

    sect->file = file;

    if (file->min_heading_depth > depth) {
	/*
	 * This heading is at a higher level than any heading we
	 * have so far placed in this file; so we set the `first'
	 * pointer.
	 */
	file->min_heading_depth = depth;
	file->first = sect;
    }

    if (file->min_heading_depth == depth)
	file->last = sect;
}

static htmlfile *html_new_file(htmlfilelist *list, char *filename)
{
    htmlfile *ret = snew(htmlfile);

    ret->next = NULL;
    if (list->tail)
	list->tail->next = ret;
    else
	list->head = ret;
    list->tail = ret;

    ret->filename = html_sanitise_filename(list, dupstr(filename));
    add234(list->files, ret->filename);
    ret->last_fragment_number = 0;
    ret->min_heading_depth = INT_MAX;
    ret->first = ret->last = NULL;

    return ret;
}

static htmlsect *html_new_sect(htmlsectlist *list, paragraph *title,
			       htmlconfig *cfg)
{
    htmlsect *ret = snew(htmlsect);

    ret->next = NULL;
    if (list->tail)
	list->tail->next = ret;
    else
	list->head = ret;
    list->tail = ret;

    ret->title = title;
    ret->file = NULL;
    ret->parent = NULL;
    ret->type = NORMAL;

    ret->fragments = snewn(cfg->ntfragments, char *);
    {
	int i;
	for (i=0; i < cfg->ntfragments; i++)
	    ret->fragments[i] = NULL;
    }

    return ret;
}

static void html_words(htmloutput *ho, word *words, int flags,
		       htmlfile *file, keywordlist *keywords, htmlconfig *cfg)
{
    word *w;
    char *c, *c2, *p, *q;
    int style, type;

    for (w = words; w; w = w->next) switch (w->type) {
      case word_HyperLink:
	if (flags & LINKS) {
	    element_open(ho, "a");
	    c = utoa_dup(w->text, CS_ASCII);
	    c2 = snewn(1 + 10*strlen(c), char);
	    for (p = c, q = c2; *p; p++) {
		if (*p == '&')
		    q += sprintf(q, "&amp;");
		else if (*p == '<')
		    q += sprintf(q, "&lt;");
		else if (*p == '>')
		    q += sprintf(q, "&gt;");
		else
		    *q++ = *p;
	    }
	    *q = '\0';
	    element_attr(ho, "href", c2);
	    sfree(c2);
	    sfree(c);
	}
	break;
      case word_UpperXref:
      case word_LowerXref:
	if (flags & LINKS) {
	    keyword *kwl = kw_lookup(keywords, w->text);
	    paragraph *p;
	    htmlsect *s;

            /* kwl should be non-NULL in any sensible case, but if the
             * input contained an unresolvable xref then it might be
             * NULL as an after-effect of that */
	    if (kwl) {
                p = kwl->para;
                s = (htmlsect *)p->private_data;

                assert(s);

                html_href(ho, file, s->file, s->fragments[0]);
            } else {
                /* If kwl is NULL, we must still open an href to
                 * _somewhere_, because it's easier than remembering
                 * which one not to close when we come to the
                 * XrefEnd */
                html_href(ho, file, file, NULL);
            }
	}
	break;
      case word_HyperEnd:
      case word_XrefEnd:
	if (flags & LINKS)
	    element_close(ho, "a");
	break;
      case word_IndexRef:
	if (flags & INDEXENTS) {
	    htmlindexref *hr = (htmlindexref *)w->private_data;
	    html_fragment(ho, hr->fragment);
	    hr->generated = true;
	}
	break;
      case word_Normal:
      case word_Emph:
      case word_Strong:
      case word_Code:
      case word_WeakCode:
      case word_WhiteSpace:
      case word_EmphSpace:
      case word_StrongSpace:
      case word_CodeSpace:
      case word_WkCodeSpace:
      case word_Quote:
      case word_EmphQuote:
      case word_StrongQuote:
      case word_CodeQuote:
      case word_WkCodeQuote:
	style = towordstyle(w->type);
	type = removeattr(w->type);
	if (style == word_Emph &&
	    (attraux(w->aux) == attr_First ||
	     attraux(w->aux) == attr_Only) &&
	    (flags & MARKUP))
	    element_open(ho, "em");
	else if (style == word_Strong &&
	    (attraux(w->aux) == attr_First ||
	     attraux(w->aux) == attr_Only) &&
	    (flags & MARKUP))
	    element_open(ho, "strong");
	else if ((style == word_Code || style == word_WeakCode) &&
		 (attraux(w->aux) == attr_First ||
		  attraux(w->aux) == attr_Only) &&
		 (flags & MARKUP))
	    element_open(ho, "code");

	if (type == word_WhiteSpace)
	    html_text(ho, L" ");
	else if (type == word_Quote) {
	    if (quoteaux(w->aux) == quote_Open)
		html_text(ho, cfg->lquote);
	    else
		html_text(ho, cfg->rquote);
	} else {
	    if (!w->alt || cvt_ok(ho->restrict_charset, w->text))
		html_text_nbsp(ho, w->text);
	    else
		html_words(ho, w->alt, flags, file, keywords, cfg);
	}

	if (style == word_Emph &&
	    (attraux(w->aux) == attr_Last ||
	     attraux(w->aux) == attr_Only) &&
	    (flags & MARKUP))
	    element_close(ho, "em");
	else if (style == word_Strong &&
	    (attraux(w->aux) == attr_Last ||
	     attraux(w->aux) == attr_Only) &&
	    (flags & MARKUP))
	    element_close(ho, "strong");
	else if ((style == word_Code || style == word_WeakCode) &&
		 (attraux(w->aux) == attr_Last ||
		  attraux(w->aux) == attr_Only) &&
		 (flags & MARKUP))
	    element_close(ho, "code");

	break;
    }
}

static void html_codepara(htmloutput *ho, word *words)
{
    element_open(ho, "pre");
    element_open(ho, "code");
    for (; words; words = words->next) if (words->type == word_WeakCode) {
	char *open_tag;
	wchar_t *t, *e;

	t = words->text;
	if (words->next && words->next->type == word_Emph) {
	    e = words->next->text;
	    words = words->next;
	} else
	    e = NULL;

	while (e && *e && *t) {
	    int n;
	    int ec = *e;

	    for (n = 0; t[n] && e[n] && e[n] == ec; n++);

	    open_tag = NULL;
	    if (ec == 'i')
		open_tag = "em";
	    else if (ec == 'b')
		open_tag = "b";
	    if (open_tag)
		element_open(ho, open_tag);

	    html_text_limit(ho, t, n);

	    if (open_tag)
		element_close(ho, open_tag);

	    t += n;
	    e += n;
	}
	html_text(ho, t);
	html_nl(ho);
    }
    element_close(ho, "code");
    element_close(ho, "pre");
}

static void html_charset_cleanup(htmloutput *ho)
{
    char outbuf[256];
    int bytes;

    bytes = charset_from_unicode(NULL, NULL, outbuf, lenof(outbuf),
				 ho->charset, &ho->cstate, NULL);
    if (bytes > 0)
        ho->write(ho->write_ctx, outbuf, bytes);
}

static void return_mostly_to_neutral(htmloutput *ho)
{
    if (ho->state == HO_IN_EMPTY_TAG && is_xhtml(ho->ver)) {
        ho_string(ho, " />");
    } else if (ho->state == HO_IN_EMPTY_TAG || ho->state == HO_IN_TAG) {
        ho_string(ho, ">");
    }

    ho->state = HO_NEUTRAL;
}

static void return_to_neutral(htmloutput *ho)
{
    if (ho->state == HO_IN_TEXT) {
	html_charset_cleanup(ho);
    }

    return_mostly_to_neutral(ho);
}

static void element_open(htmloutput *ho, char const *name)
{
    return_to_neutral(ho);
    ho_string(ho, "<");
    ho_string(ho, name);
    ho->state = HO_IN_TAG;
}

static void element_close(htmloutput *ho, char const *name)
{
    return_to_neutral(ho);
    ho_string(ho, "</");
    ho_string(ho, name);
    ho_string(ho, ">");
    ho->state = HO_NEUTRAL;
}

static void element_empty(htmloutput *ho, char const *name)
{
    return_to_neutral(ho);
    ho_string(ho, "<");
    ho_string(ho, name);
    ho->state = HO_IN_EMPTY_TAG;
}

static void html_nl(htmloutput *ho)
{
    return_to_neutral(ho);
    ho_string(ho, "\n");
}

static void html_raw(htmloutput *ho, char *text)
{
    return_to_neutral(ho);
    ho_string(ho, text);
}

static void html_raw_as_attr(htmloutput *ho, char *text)
{
    assert(ho->state == HO_IN_TAG || ho->state == HO_IN_EMPTY_TAG);
    ho_string(ho, " ");
    ho_string(ho, text);
}

static void element_attr(htmloutput *ho, char const *name, char const *value)
{
    html_charset_cleanup(ho);
    assert(ho->state == HO_IN_TAG || ho->state == HO_IN_EMPTY_TAG);
    ho_string(ho, " ");
    ho_string(ho, name);
    ho_string(ho, "=\"");
    ho_string(ho, value);
    ho_string(ho, "\"");
}

static void element_attr_w(htmloutput *ho, char const *name,
			   wchar_t const *value)
{
    html_charset_cleanup(ho);
    ho_string(ho, " ");
    ho_string(ho, name);
    ho_string(ho, "=\"");
    html_text_limit_internal(ho, value, 0, true, false);
    html_charset_cleanup(ho);
    ho_string(ho, "\"");
}

static void html_text(htmloutput *ho, wchar_t const *text)
{
    return_mostly_to_neutral(ho);
    html_text_limit_internal(ho, text, 0, false, false);
}

static void html_text_nbsp(htmloutput *ho, wchar_t const *text)
{
    return_mostly_to_neutral(ho);
    html_text_limit_internal(ho, text, 0, false, true);
}

static void html_text_limit(htmloutput *ho, wchar_t const *text, int maxlen)
{
    return_mostly_to_neutral(ho);
    html_text_limit_internal(ho, text, maxlen, false, false);
}

static void html_text_limit_internal(htmloutput *ho, wchar_t const *text,
				     int maxlen, bool quote_quotes, bool nbsp)
{
    int textlen = ustrlen(text);
    char outbuf[256];
    int bytes;
    bool err;

    if (ho->hackflags & (HO_HACK_QUOTEQUOTES | HO_HACK_OMITQUOTES))
	quote_quotes = true;	       /* override the input value */

    if (maxlen > 0 && textlen > maxlen)
	textlen = maxlen;
    if (ho->hacklimit >= 0) {
	if (textlen > ho->hacklimit)
	    textlen = ho->hacklimit;
	ho->hacklimit -= textlen;
    }

    while (textlen > 0) {
	/* Scan ahead for characters we really can't display in HTML. */
	int lenbefore, lenafter;
	for (lenbefore = 0; lenbefore < textlen; lenbefore++)
	    if (text[lenbefore] == L'<' ||
		text[lenbefore] == L'>' ||
		text[lenbefore] == L'&' ||
		(text[lenbefore] == L'"' && quote_quotes) ||
		(text[lenbefore] == L' ' && nbsp))
		break;
	lenafter = lenbefore;
	bytes = charset_from_unicode(&text, &lenafter, outbuf, lenof(outbuf),
				     ho->charset, &ho->cstate, &err);
	textlen -= (lenbefore - lenafter);
	if (bytes > 0)
            ho->write(ho->write_ctx, outbuf, bytes);
	if (err) {
	    /*
	     * We have encountered a character that cannot be
	     * displayed in the selected output charset. Therefore,
	     * we use an HTML numeric entity reference.
	     */
            char buf[40];
	    assert(textlen > 0);
            sprintf(buf, "&#%ld;", (long int)*text);
            ho_string(ho, buf);
	    text++, textlen--;
	} else if (lenafter == 0 && textlen > 0) {
	    /*
	     * We have encountered a character which is special to
	     * HTML.
	     */
            if (*text == L'"' && (ho->hackflags & HO_HACK_OMITQUOTES)) {
                ho_string(ho, "'");
            } else if (ho->hackflags & HO_HACK_QUOTENOTHING) {
                char c = *text;
                ho->write(ho->write_ctx, &c, 1);
            } else {
                if (*text == L'<')
                    ho_string(ho, "&lt;");
                else if (*text == L'>')
                    ho_string(ho, "&gt;");
                else if (*text == L'&')
                    ho_string(ho, "&amp;");
                else if (*text == L'"')
                    ho_string(ho, "&quot;");
                else if (*text == L' ') {
                    assert(nbsp);
                    ho_string(ho, "&nbsp;");
                } else
                    assert(!"Can't happen");
            }
	    text++, textlen--;
	}
    }
}

static void cleanup(htmloutput *ho)
{
    return_to_neutral(ho);
    ho_finish(ho);
}

static void html_href(htmloutput *ho, htmlfile *thisfile,
		      htmlfile *targetfile, char *targetfrag)
{
    rdstringc rs = { 0, 0, NULL };
    char *url;

    if (targetfile != thisfile)
	rdaddsc(&rs, targetfile->filename);
    if (targetfrag) {
	rdaddc(&rs, '#');
	rdaddsc(&rs, targetfrag);
    }

    /* If _neither_ of those conditions were true, we don't have a URL
     * at all and will segfault when we pass url==NULL to element_attr.
     *
     * I think this can only occur as a knock-on effect from an input
     * file error, but we still shouldn't crash, of course. */

    url = rs.text;

    element_open(ho, "a");
    if (url) {
        element_attr(ho, "href", url);
        sfree(url);
    }
}

static void html_fragment(htmloutput *ho, char const *fragment)
{
    element_open(ho, "a");
    element_attr(ho, "name", fragment);
    if (is_xhtml(ho->ver))
	element_attr(ho, "id", fragment);
    element_close(ho, "a");
}

static char *html_format(paragraph *p, char *template_string)
{
    char *c, *t;
    word *w;
    wchar_t *ws, wsbuf[2];
    rdstringc rs = { 0, 0, NULL };

    t = template_string;
    while (*t) {
	if (*t == '%' && t[1]) {
	    int fmt;

	    t++;
	    fmt = *t++;

	    if (fmt == '%') {
		rdaddc(&rs, fmt);
		continue;
	    }

	    w = NULL;
	    ws = NULL;

	    if (p->kwtext && fmt == 'n')
		w = p->kwtext;
	    else if (p->kwtext2 && fmt == 'b') {
		/*
		 * HTML fragment names must start with a letter, so
		 * simply `1.2.3' is not adequate. In this case I'm
		 * going to cheat slightly by prepending the first
		 * character of the first word of kwtext, so that
		 * we get `C1' for chapter 1, `S2.3' for section
		 * 2.3 etc.
		 */
		if (p->kwtext && p->kwtext->text[0]) {
		    ws = wsbuf;
		    wsbuf[1] = '\0';
		    wsbuf[0] = p->kwtext->text[0];
		}
		w = p->kwtext2;
	    } else if (p->keyword && *p->keyword && fmt == 'k')
		ws = p->keyword;
	    else
		/* %N comes here; also failure cases of other fmts */
		w = p->words;

	    if (ws) {
		c = utoa_dup(ws, CS_ASCII);
		rdaddsc(&rs,c);
		sfree(c);
	    }

	    while (w) {
		if (removeattr(w->type) == word_Normal) {
		    c = utoa_dup(w->text, CS_ASCII);
		    rdaddsc(&rs,c);
		    sfree(c);
		}
		w = w->next;
	    }
	} else {
	    rdaddc(&rs, *t++);
	}
    }

    return rdtrimc(&rs);
}

static char *html_sanitise_fragment(htmlfilelist *files, htmlfile *file,
				    char *text)
{
    /*
     * The HTML 4 spec's strictest definition of fragment names (<a
     * name> and "id" attributes) says that they `must begin with a
     * letter and may be followed by any number of letters, digits,
     * hyphens, underscores, colons, and periods'.
     * 
     * So here we unceremoniously rip out any characters not
     * conforming to this limitation.
     */
    char *p = text, *q = text;

    while (*p && !((*p>='A' && *p<='Z') || (*p>='a' && *p<='z')))
	p++;
    if ((*q++ = *p++) != '\0') {
	while (*p) {
	    if ((*p>='A' && *p<='Z') ||
		(*p>='a' && *p<='z') ||
		(*p>='0' && *p<='9') ||
		*p=='-' || *p=='_' || *p==':' || *p=='.')
		*q++ = *p;
	    p++;
	}

	*q = '\0';
    }

    /* If there's nothing left, make something valid up */
    if (!*text) {
	static const char anonfrag[] = "anon";
	text = sresize(text, lenof(anonfrag), char);
	strcpy(text, anonfrag);
    }

    /*
     * Now we check for clashes with other fragment names, and
     * adjust this one if necessary by appending a hyphen followed
     * by a number.
     */
    {
	htmlfragment *frag = snew(htmlfragment);
	int len = 0;		       /* >0 indicates we have resized */
	int suffix = 1;

	frag->file = file;
	frag->fragment = text;

	while (add234(files->frags, frag) != frag) {
	    if (!len) {
		len = strlen(text);
		frag->fragment = text = sresize(text, len+20, char);
	    }

	    sprintf(text + len, "-%d", ++suffix);
	}
    }

    return text;
}

static char *html_sanitise_filename(htmlfilelist *files, char *text)
{
    /*
     * Unceremoniously rip out any character that might cause
     * difficulty in some filesystem or another, or be otherwise
     * inconvenient.
     * 
     * That doesn't leave much punctuation. I permit alphanumerics
     * and +-.=_ only.
     */
    char *p = text, *q = text;

    while (*p) {
	if ((*p>='A' && *p<='Z') ||
	    (*p>='a' && *p<='z') ||
	    (*p>='0' && *p<='9') ||
	    *p=='-' || *p=='_' || *p=='+' || *p=='.' || *p=='=')
	    *q++ = *p;
	p++;
    }
    *q = '\0';

    /* If there's nothing left, make something valid up */
    if (!*text) {
	static const char anonfrag[] = "anon.html";
	text = sresize(text, lenof(anonfrag), char);
	strcpy(text, anonfrag);
    }

    /*
     * Now we check for clashes with other filenames, and adjust
     * this one if necessary by appending a hyphen followed by a
     * number just before the file extension (if any).
     */
    {
	int len, extpos;
	int suffix = 1;

	p = NULL;

	while (find234(files->files, text)) {
	    if (!p) {
		len = strlen(text);
		p = text;
		text = snewn(len+20, char);

		for (extpos = len; extpos > 0 && p[extpos-1] != '.'; extpos--);
		if (extpos > 0)
		    extpos--;
		else
		    extpos = len;
	    }

	    sprintf(text, "%.*s-%d%s", extpos, p, ++suffix, p+extpos);
	}

	if (p)
	    sfree(p);
    }

    return text;
}

static void html_contents_entry(htmloutput *ho, int depth, htmlsect *s,
				htmlfile *thisfile, keywordlist *keywords,
				htmlconfig *cfg)
{
    if (ho->contents_level >= depth && ho->contents_level > 0) {
	element_close(ho, "li");
	html_nl(ho);
    }

    while (ho->contents_level > depth) {
	element_close(ho, "ul");
	ho->contents_level--;
	if (ho->contents_level > 0) {
	    element_close(ho, "li");
	}
	html_nl(ho);
    }

    while (ho->contents_level < depth) {
	html_nl(ho);
	element_open(ho, "ul");
	html_nl(ho);
	ho->contents_level++;
    }

    if (!s)
	return;

    element_open(ho, "li");
    html_href(ho, thisfile, s->file, s->fragments[0]);
    html_section_title(ho, s, thisfile, keywords, cfg, false);
    element_close(ho, "a");
    /* <li> will be closed by a later invocation */
}

static void html_section_title(htmloutput *ho, htmlsect *s, htmlfile *thisfile,
			       keywordlist *keywords, htmlconfig *cfg,
			       bool real)
{
    if (s->title) {
	sectlevel *sl;
	word *number;
	int depth = heading_depth(s->title);

	if (depth < 0)
	    sl = NULL;
	else if (depth == 0)
	    sl = &cfg->achapter;
	else if (depth <= cfg->nasect)
	    sl = &cfg->asect[depth-1];
	else
	    sl = &cfg->asect[cfg->nasect-1];

	if (!sl || !sl->number_at_all)
	    number = NULL;
	else if (sl->just_numbers)
	    number = s->title->kwtext2;
	else
	    number = s->title->kwtext;

	if (number) {
	    html_words(ho, number, MARKUP,
		       thisfile, keywords, cfg);
	    html_text(ho, sl->number_suffix);
	}

	html_words(ho, s->title->words, real ? ALL : MARKUP,
		   thisfile, keywords, cfg);
    } else {
	assert(s->type != NORMAL);
	/*
	 * If we're printing the full document title for _real_ and
	 * there isn't one, we don't want to print `Preamble' at
	 * the top of what ought to just be some text. If we need
	 * it in any other context such as TOCs, we need to print
	 * `Preamble'.
	 */
	if (s->type == TOP && !real)
	    html_text(ho, cfg->preamble_text);
	else if (s->type == INDEX)
	    html_text(ho, cfg->index_text);
    }
}
