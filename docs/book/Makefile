# The venue book.  LuaLaTeX for real OpenType fonts; two passes so that
# cross-references and the table of contents resolve.
LATEX  := lualatex -interaction=nonstopmode -halt-on-error
JOB    := book
CHECK  := ../../scripts/check_book_requirements.py

.PHONY: all check clean

all: $(JOB).pdf

$(JOB).pdf: $(shell find . -name '*.tex' -o -name '*.sty')
	$(LATEX) $(JOB).tex
	makeglossaries $(JOB)
	$(LATEX) $(JOB).tex
	$(LATEX) $(JOB).tex

check: $(JOB).pdf
	python3 $(CHECK) $(JOB).req

clean:
	rm -f $(JOB).pdf $(JOB).aux $(JOB).log $(JOB).toc $(JOB).out $(JOB).req
	rm -f $(JOB).glo $(JOB).gls $(JOB).glg $(JOB).acn $(JOB).acr $(JOB).alg $(JOB).ist
	rm -f */*.aux */*/*.aux
