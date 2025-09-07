"""
Report the differences between a previous and a current clang-tidy summary by file.
These changes can be either improvements (category no longer appears or has a reduced count)
or a worsening (new category of error or an increased count).
"""

import sys
import argparse
import logging

logging.basicConfig(level=logging.DEBUG, format='%(asctime)s %(levelname)s:: %(message)s')
LOGGER = logging.getLogger(__name__)

class MyException(Exception):
    """Application-specific exception so can be distinguished from RuntimeError"""

def report_file_differences(filename, previous_file_summary, current_file_summary):
    """Report differences for the categories for the file, getting worse or better"""
    # Report as fixed any categories in the previous that are not in the current.
    for previous_key in previous_file_summary:
        if previous_key not in current_file_summary:
            print(f'{filename} {previous_key} is fixed')

    for current_key in current_file_summary.keys():
        if current_key in previous_file_summary:
            # For all the common categories, report if things have got worse or better.
            if current_key == 'line':
                continue
            if current_file_summary[current_key] > previous_file_summary[current_key]:
                print(f'Error: {filename} {current_key} is worse, was {previous_file_summary[current_key]}, is now {current_file_summary[current_key]}')
            elif current_file_summary[current_key] < previous_file_summary[current_key]:
                print(f'{filename} {current_key} is better, was {previous_file_summary[current_key]}, is now {current_file_summary[current_key]}')
        else:
            # Report as new errors, any categories in the current that are not in the previous
            print(f'Error: {filename} {current_key} has {current_file_summary[current_key]} violations that were not there before.')

def is_changed(previous_summary, current_summary):
    """Returns true if the previous and current summaries have changes"""
    changed = True
    if len(current_summary.keys()) == len(previous_summary.keys()):
        changed = False
        for current_filename in current_summary.keys():
            if not current_filename in previous_summary:
                changed = True
                break
            previous_file_summary = previous_summary[current_filename]
            current_file_summary = current_summary[current_filename]
            if previous_file_summary['line'] != current_file_summary['line']:
                changed = True
                break
    return changed

def read_summary_file(s_filename):
    """Read summary by file and build structure for comparision"""
    summary_data = {}
    with open(s_filename, 'r') as file_handle:
        lines = [line.strip() for line in file_handle]
    line_number = 0
    for line in lines:
        line_number = line_number + 1
        if not line or line[0] == '#':
            continue
        tokens = line.split('|')
        if len(tokens) != 2:
            message = 'Error in file {}, line {} needs to have exactly two tokens separated by pipe.'.format(filename, line_number)
            logging.error(message)
            raise MyException(message)
        filename = tokens[0]
        categories = tokens[1].split(';')
        file_summary = {}
        file_summary['line'] = line
        for category_and_score in categories:
            tokens = category_and_score.split(':')
            if len(tokens) < 2:
                print(f'{s_filename}:{line_number} parsing error for category {category_and_score}')
                continue
            category = tokens[0]
            score = tokens[1]
            file_summary[category] = score
        summary_data[filename] = file_summary
    return summary_data

class Reporter:
    """Produce a report of the differences between two clang tidy report by-file summary files"""
    def __init__(self, **kwargs):
        self.previous_file = kwargs.get('previous')
        self.current_file = kwargs.get('current')
        self.verbose = kwargs.get('verbose')

    def report(self):
        """Read the previous and current file-based summaries and report where things have changed."""
        previous_summary = read_summary_file(self.previous_file.name)
        current_summary = read_summary_file(self.current_file.name)
        self.report_differences(previous_summary, current_summary)

    def report_differences(self, previous_summary, current_summary):
        """Report the differences between current, present in one, absent in the other, and changes in counts"""
        if not current_summary.keys() and not previous_summary.keys():
            print('Both summaries are empty.')
            return

        if not current_summary.keys():
            print('All issues are fixed in the current report summary.')
            return

        if not is_changed(previous_summary, current_summary):
            print('Current report is unchanged from previous.')
            return

        for previous_filename in previous_summary.keys():
            if not previous_filename in current_summary:
                print(f'All issues in {previous_filename} have now been fixed.')

        for current_filename in current_summary.keys():
            if not current_filename in previous_summary:
                print(f'Error: File {current_filename} is a new file with violation(s)')
                continue
            previous_file_summary = previous_summary[current_filename]
            current_file_summary = current_summary[current_filename]
            if previous_file_summary['line'] == current_file_summary['line']:
                if self.verbose:
                    print(f'File {current_filename} violations are unchanged.')
                continue
            report_file_differences(current_filename, previous_file_summary, current_file_summary)

def main():
    """Main entrypoint for running as standalone script."""
    parser = argparse.ArgumentParser(description=('Analyses two clang-tidy by-file summary reports to show the difference in issues found.'))
    parser.add_argument('--previous', action='store', required=True, type=argparse.FileType('r'),
                        help='The name of the file that contains the previous report.')
    parser.add_argument('--current', action='store', required=True, type=argparse.FileType('r'),
                        help='The name of the file that contains the current report.')
    parser.add_argument('--verbose', action='store_true', help='Verbose output, includes it showing checks that have not changed.')
    args = parser.parse_args()
    reporter = Reporter(previous=args.previous, current=args.current, verbose=args.verbose)

    exit_status = 0
    try:
        reporter.report()
    except MyException as ex:
        exit_status = -1
        if str(ex):
            LOGGER.error(str(ex))

    if exit_status == 0:
        LOGGER.info('sca-summary: Report completed.')
    else:
        LOGGER.info('Error: sca-summary report failed.')
    sys.exit(exit_status)

    reporter.report()

if __name__ == '__main__':
    main()
