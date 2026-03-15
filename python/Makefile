lint:
	pylint dsl

test:
	pytest -q

coverage:
	coverage run -m pytest
	coverage report
	coverage html

all: lint test coverage
