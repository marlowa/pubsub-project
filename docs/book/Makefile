# The venue book.  LuaLaTeX for real OpenType fonts; two passes so that
# cross-references and the table of contents resolve.
LATEX  := lualatex -interaction=nonstopmode -halt-on-error
JOB    := book
CHECK  := ../../scripts/check_book_requirements.py

.PHONY: all check clean text

all: $(JOB).pdf

$(JOB).pdf: $(shell find . -name '*.tex' -o -name '*.sty')
	$(LATEX) $(JOB).tex
	makeglossaries $(JOB)
	$(LATEX) $(JOB).tex
	$(LATEX) $(JOB).tex

check: $(JOB).pdf
	python3 $(CHECK) $(JOB).req

# A plain-text copy, for tools that will not take a PDF.  Goes through HTML so
# that no page headers, footers or contents leaders appear in the text.
text: $(JOB).txt

$(JOB).txt: $(JOB).pdf
	make4ht -u $(JOB).tex >/dev/null 2>&1 || true
	pandoc -f html -t plain --wrap=preserve $(JOB).html -o $(JOB).txt
	sed -i '/^refextract$$/d' $(JOB).txt

clean:
	rm -f $(JOB).pdf $(JOB).aux $(JOB).log $(JOB).toc $(JOB).out $(JOB).req
	rm -f $(JOB).txt $(JOB).html $(JOB).css $(JOB).lg $(JOB).xref $(JOB).tmp $(JOB).idv $(JOB).4ct $(JOB).4tc $(JOB).dvi
	rm -f $(JOB).glo $(JOB).gls $(JOB).glg $(JOB).acn $(JOB).acr $(JOB).alg $(JOB).ist
	rm -f */*.aux */*/*.aux
