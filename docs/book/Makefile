# The venue book.  LuaLaTeX for real OpenType fonts; two passes so that
# cross-references and the table of contents resolve.
LATEX  := lualatex -interaction=nonstopmode -halt-on-error
JOB    := book
CHECK  := ../../scripts/check_book_requirements.py
CHECK_EPUB := ../../scripts/check_epub.py

.PHONY: all check clean text epub tools-for-the-pdf tools-for-the-epub tools-for-the-text

# What each output needs from TeX Live and from the system. The book is built on a machine with a current TeX Live,
# such as TeX Live 2023: RHEL 8 and Rocky 8 provide no package holding glossaries, makeglossaries, cleveref, tex4ebook
# or make4ht, so the book cannot be built there from the distribution's own packages. Each check below names
# everything missing and stops before LaTeX runs, rather than letting the first missing package stop a LaTeX pass.
# The packages are those the book's .tex and .sty files load; a package added there is added here too.
PDF_COMMANDS  := lualatex makeglossaries bibtex
PDF_PACKAGES  := amsmath booktabs cleveref enumitem fontenc geometry glossaries hyperref lmodern longtable microtype \
                 tcolorbox xcolor
EPUB_COMMANDS := tex4ebook
TEXT_COMMANDS := make4ht pandoc

# Prints each missing command and package after the given heading, and fails if any is missing.
define require_tools
	@missing=""; \
	for command in $(1); do command -v $$command >/dev/null 2>&1 || missing="$$missing $$command"; done; \
	for package in $(2); do kpsewhich $$package.sty >/dev/null 2>&1 || missing="$$missing $$package.sty"; done; \
	if [ -n "$$missing" ]; then \
	  echo "The book's $(3) cannot be built on this machine, which lacks:$$missing"; \
	  echo "Build the book on a machine with a current TeX Live, such as TeX Live 2023. RHEL 8 and Rocky 8 provide"; \
	  echo "no package holding glossaries, makeglossaries, cleveref, tex4ebook or make4ht."; \
	  exit 1; \
	fi
endef

tools-for-the-pdf:
	$(call require_tools,$(PDF_COMMANDS),$(PDF_PACKAGES),PDF)

tools-for-the-epub:
	$(call require_tools,$(EPUB_COMMANDS),,EPUB)

tools-for-the-text:
	$(call require_tools,$(TEXT_COMMANDS),,plain-text copy)

# The PDF and the EPUB together: building the book builds both.
all: $(JOB).pdf $(JOB).epub

# The last pass's log and list of requirements are kept as $(JOB)-pdf.log and $(JOB)-pdf.req,
# because converting the book to EPUB or HTML runs LaTeX again under the same name and writes
# $(JOB).log and $(JOB).req afresh. "make check" reads the copies, so it checks the PDF's pass
# whatever was built after it.
$(JOB).pdf: $(shell find . -name '*.tex' -o -name '*.sty' -o -name '*.bib') | tools-for-the-pdf
	$(LATEX) $(JOB).tex
	makeglossaries $(JOB)
	$(LATEX) $(JOB).tex
	bibtex $(JOB)
	$(LATEX) $(JOB).tex
	$(LATEX) $(JOB).tex
	cp $(JOB).log $(JOB)-pdf.log
	cp $(JOB).req $(JOB)-pdf.req

# The EPUB is checked by reading it, not from what tex4ebook printed, which always includes the errors
# described at $(JOB).epub below: check_epub.py requires a valid EPUB container and every page to parse as XML.
check: $(JOB).pdf $(JOB).epub
	@! grep -q 'Reference.*undefined\|Citation.*undefined' $(JOB)-pdf.log || \
	  { echo "undefined cross-references:"; grep -o "Reference \`[^']*' on page [0-9]*" $(JOB)-pdf.log; exit 1; }
	@! grep -q 'multiply defined' $(JOB)-pdf.log || \
	  { echo "duplicate labels:"; grep -o "Label \`[^']*' multiply defined" $(JOB)-pdf.log; exit 1; }
	python3 $(CHECK) $(JOB)-pdf.req
	python3 $(CHECK_EPUB) $(JOB).epub

# A plain-text copy, for tools that will not take a PDF.  Goes through HTML so
# that no page headers, footers or contents leaders appear in the text.
text: $(JOB).txt

$(JOB).txt: $(JOB).pdf $(JOB).mk4 | tools-for-the-text
	make4ht -u $(JOB).tex >/dev/null 2>&1 || true
	pandoc -f html -t plain --wrap=preserve $(JOB).html -o $(JOB).txt

# An EPUB 3 copy, for e-book readers.  Built after the PDF, because TeX4ht reads the
# glossary, bibliography and cross-reference files that the PDF's passes write.  The
# conversion always reports the error "Missing \begin{document}", once for each of its
# LaTeX passes, from TeX4ht's own support for the glossaries package; $(JOB).mk4 removes
# the stray word it concerns.  So the conversion fails only on any other error, or if no
# EPUB was written, and "make check" then reads the EPUB to confirm it is sound.
epub: $(JOB).epub

$(JOB).epub: $(JOB).pdf $(JOB).mk4 | tools-for-the-epub
	rm -f $(JOB).epub
	tex4ebook -e $(JOB).mk4 -f epub3 $(JOB).tex > $(JOB)-epub-conversion.log 2>&1 || true
	@! grep '^! ' $(JOB).log | grep -v 'Missing \\begin{document}' || { echo "errors converting the book to EPUB: see $(JOB).log"; exit 1; }
	@test -s $(JOB).epub || { echo "tex4ebook wrote no $(JOB).epub"; exit 1; }
	@printf '%s\n' "EPUB generated. TeX4ht's known glossary diagnostic and any tidy warning are in $(JOB)-epub-conversion.log. Run 'make check' to validate $(JOB).epub."

clean:
	rm -f $(JOB).pdf $(JOB).aux $(JOB).bbl $(JOB).blg $(JOB).log $(JOB).toc $(JOB).out $(JOB).req
	rm -f $(JOB)-pdf.log $(JOB)-pdf.req $(JOB)-epub-conversion.log
	rm -f $(JOB).txt $(JOB).html $(JOB).css $(JOB).lg $(JOB).xref $(JOB).tmp $(JOB).idv $(JOB).4ct $(JOB).4tc $(JOB).dvi
	rm -f $(JOB).glo $(JOB).gls $(JOB).glg $(JOB).acn $(JOB).acr $(JOB).alg $(JOB).ist
	rm -f $(JOB).epub $(JOB).ncx content.opf *.xhtml
	rm -rf $(JOB)-epub3
	rm -f */*.aux */*/*.aux
