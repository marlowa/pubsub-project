.PHONY: lint test coverage all

lint:
	pylint dsl

test:
	python3 -m pytest -q

coverage:
	python3 -m coverage run -m pytest
	python3 -m coverage report
	python3 -m coverage html

all: lint test coverage
