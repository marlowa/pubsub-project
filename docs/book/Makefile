# The venue book.  LuaLaTeX for real OpenType fonts; two passes so that
# cross-references and the table of contents resolve.
LATEX  := lualatex -interaction=nonstopmode -halt-on-error
JOB    := book
CHECK  := ../../scripts/check_book_requirements.py

.PHONY: all check clean text epub

# The PDF and the EPUB together: building the book builds both.
all: $(JOB).pdf $(JOB).epub

# The last pass's log and list of requirements are kept as $(JOB)-pdf.log and $(JOB)-pdf.req,
# because converting the book to EPUB or HTML runs LaTeX again under the same name and writes
# $(JOB).log and $(JOB).req afresh. "make check" reads the copies, so it checks the PDF's pass
# whatever was built after it.
$(JOB).pdf: $(shell find . -name '*.tex' -o -name '*.sty' -o -name '*.bib')
	$(LATEX) $(JOB).tex
	makeglossaries $(JOB)
	$(LATEX) $(JOB).tex
	bibtex $(JOB)
	$(LATEX) $(JOB).tex
	$(LATEX) $(JOB).tex
	cp $(JOB).log $(JOB)-pdf.log
	cp $(JOB).req $(JOB)-pdf.req

check: $(JOB).pdf
	@! grep -q 'Reference.*undefined\|Citation.*undefined' $(JOB)-pdf.log || \
	  { echo "undefined cross-references:"; grep -o "Reference \`[^']*' on page [0-9]*" $(JOB)-pdf.log; exit 1; }
	@! grep -q 'multiply defined' $(JOB)-pdf.log || \
	  { echo "duplicate labels:"; grep -o "Label \`[^']*' multiply defined" $(JOB)-pdf.log; exit 1; }
	python3 $(CHECK) $(JOB)-pdf.req

# A plain-text copy, for tools that will not take a PDF.  Goes through HTML so
# that no page headers, footers or contents leaders appear in the text.
text: $(JOB).txt

$(JOB).txt: $(JOB).pdf $(JOB).mk4
	make4ht -u $(JOB).tex >/dev/null 2>&1 || true
	pandoc -f html -t plain --wrap=preserve $(JOB).html -o $(JOB).txt

# An EPUB 3 copy, for e-book readers.  Built after the PDF, because TeX4ht reads the
# glossary, bibliography and cross-reference files that the PDF's passes write.  The
# conversion always reports one error, from TeX4ht's own support for the glossaries
# package (see $(JOB).mk4), so it fails only on any other error, or if no EPUB was written.
epub: $(JOB).epub

$(JOB).epub: $(JOB).pdf $(JOB).mk4
	rm -f $(JOB).epub
	tex4ebook -e $(JOB).mk4 -f epub3 $(JOB).tex || true
	@! grep '^! ' $(JOB).log | grep -v 'Missing \\begin{document}' || { echo "errors converting the book to EPUB: see $(JOB).log"; exit 1; }
	@test -s $(JOB).epub || { echo "tex4ebook wrote no $(JOB).epub"; exit 1; }

clean:
	rm -f $(JOB).pdf $(JOB).aux $(JOB).bbl $(JOB).blg $(JOB).log $(JOB).toc $(JOB).out $(JOB).req
	rm -f $(JOB)-pdf.log $(JOB)-pdf.req
	rm -f $(JOB).txt $(JOB).html $(JOB).css $(JOB).lg $(JOB).xref $(JOB).tmp $(JOB).idv $(JOB).4ct $(JOB).4tc $(JOB).dvi
	rm -f $(JOB).glo $(JOB).gls $(JOB).glg $(JOB).acn $(JOB).acr $(JOB).alg $(JOB).ist
	rm -f $(JOB).epub $(JOB).ncx content.opf *.xhtml
	rm -rf $(JOB)-epub3
	rm -f */*.aux */*/*.aux
