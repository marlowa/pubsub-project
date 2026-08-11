"""
Script to check the results of doing a clang-tidy and summarise results by category and by file.
"""
import os
import sys
import argparse
import re
import logging

logging.basicConfig(level=logging.DEBUG, format='%(asctime)s %(levelname)s:: %(message)s')
LOGGER = logging.getLogger(__name__)

class MyException(Exception):
    """Application-specific exception so can be distinguished from RuntimeError"""

def get_filename_and_folder(filename):
    """Return the uppercased leafname from the specified filename"""
    leafname = os.path.basename(filename)
    fileandfolders = filename.split('\\')
    if len(fileandfolders) >= 2: # drive letter and DOS delimiter
        leafname = fileandfolders[-2] + '_' + leafname
    else:
        fileandfolders = filename.split('/')
        if len(fileandfolders) >= 2:
            leafname = fileandfolders[-2] + '_' + leafname
    return leafname.upper()

def get_fullname_from_filenameswithfolder(filename, filenameswithfolder):
    """Return the full filename by looking up leafname in supplied list"""
    leafname = get_filename_and_folder(filename)
    if leafname in filenameswithfolder:
        filename = filenameswithfolder[leafname]
    return filename

def remove_dot_and_dot_dot_from_path(filename):
    """pathname resolved so that it does not contain dot or dot-dot"""
    bfwd = True
    folderstack = filename.split('/')
    if len(folderstack) < 2:
        bfwd = False
        folderstack = filename.split('\\')
    if len(folderstack) >= 2:
        for i in range(0, len(folderstack)):
            if folderstack[i] == '..' and i >= 2:
                del folderstack[i]
                del folderstack[i-1]
                break
            if folderstack[i] == '.' and i >= 2:
                del folderstack[i]
                break
        if bfwd:
            filename = '/'.join(folderstack)
        else:
            filename = '\\'.join(folderstack)
    return filename

def find_all_leafnames():
    """Find all the header filenames and cpp filenames and return a map of them where the value is the pathname relative to the current directory"""
    leafnameswithfolder = {}
    for root, _dirs, filenames in os.walk('.'):
        if filenames:
            for filename in filenames:
                if re.match(r'.*\.cpp$', filename) or re.match(r'.*\.h$', filename):
                    full_name = os.path.join(root, filename)
                    full_name = os.path.join(root, filename)
                    if full_name.startswith('./') or full_name.startswith('.\\'):
                        full_name = full_name[2:]
                    leafnameswithfolder[get_filename_and_folder(full_name)] = full_name
    return leafnameswithfolder

class ScaSummary:
    """Analyse output from clang-tidy and produce summary report."""
    def __init__(self, **kwargs):
        self.file = kwargs.get('file', -1)
        self.output = kwargs.get('output', -1)
        self.output_byfile = kwargs.get('output_byfile', -1)
        # report the left warning as the right
        self.override_categories = [
            '[hicpp-use-override]',
            '[cppcoreguidelines-explicit-virtual-functions]',
            '[modernize-use-override]'
        ]
        self.nullptr_categories = [
            '[hicpp-use-nullptr]',
            '[modernize-use-nullptr]'
        ]
        self.narrowing_conversions_categories = [
            '[cppcoreguidelines-narrowing-conversions]',
            '[bugprone-narrowing-conversions]'
        ]
        self.member_init_categories = [
            '[hicpp-member-init]',
            '[cppcoreguidelines-pro-type-member-init]'
        ]
        self.anon_namespaces_categories = [
            '[google-build-namespaces]',
            '[fuchsia-header-anon-namespaces]',
            '[cert-dcl59-cpp]'
        ]
        self.bugprone_unhandled_self_assignment = [
            '[bugprone-unhandled-self-assignment]',
            '[cert-oop54-cpp]'
        ]
        self.readability_uppercase_literal_suffix = [
            '[readability-uppercase-literal-suffix]',
            '[hicpp-uppercase-literal-suffix]',
            '[cert-dcl16-c]'
        ]
        self.misc_non_private_member_variables_in_classes = [
            '[misc-non-private-member-variables-in-classes]',
            '[cppcoreguidelines-non-private-member-variables-in-classes]'
        ]
        self.modernize_avoid_c_arrays = [
            '[modernize-avoid-c-arrays]',
            '[hicpp-avoid-c-arrays]',
            '[cppcoreguidelines-avoid-c-arrays]'
        ]
        self.catmap = {
            '[hicpp-member-init]'                                         : '[cppcoreguidelines-pro-type-member-init]',
            '[cppcoreguidelines-narrowing-conversions]'                   : '[bugprone-narrowing-conversions]',
            '[cppcoreguidelines-explicit-virtual-functions]'              : '[modernize-use-override]',
            '[hicpp-use-override]'                                        : '[modernize-use-override]',
            '[hicpp-avoid-goto]'                                          : '[cppcoreguidelines-avoid-goto]',
            '[cert-err09-cpp]'                                            : '[misc-throw-by-value-catch-by-reference]',
            '[cert-err61-cpp]'                                            : '[misc-throw-by-value-catch-by-reference]',
            '[hicpp-no-malloc]'                                           : '[cppcoreguidelines-no-malloc]',
            '[cert-msc51-cpp]'                                            : '[cert-msc32-c]',
            '[hicpp-uppercase-literal-suffix]'                            : '[readability-uppercase-literal-suffix]',
            '[cert-dcl16-c]'                                              : '[readability-uppercase-literal-suffix]',
            '[hicpp-use-nullptr]'                                         : '[modernize-use-nullptr]',
            '[google-readability-casting]'                                : '[cppcoreguidelines-pro-type-cstyle-cast]',
            '[fuchsia-header-anon-namespaces]'                            : '[google-build-namespaces]',
            '[cert-dcl59-cpp]'                                            : '[google-build-namespaces]',
            '[cert-oop54-cpp]'                                            : '[bugprone-unhandled-self-assignment]',
            '[cppcoreguidelines-non-private-member-variables-in-classes]' : '[misc-non-private-member-variables-in-classes]',
            '[hicpp-avoid-c-arrays]'                                      : '[modernize-avoid-c-arrays]',
            '[cppcoreguidelines-avoid-c-arrays]'                          : '[modernize-avoid-c-arrays]'
        }

        self.failed_files = kwargs.get('failed_files')

        if self.output_byfile is None:
            self.output_byfile = 'clang-check-summary-report-byfile.txt'
        self.failed_files_handle = None
        self.failed_filenames = []

        # The pattern is: sourceFilename colon digits colon digits anything square bracket anything square backet
        self.expression = r"""(.*\.(?:cpp|cc|hh|h)):([0-9]*):[0-9]*:.*(\[.*\])"""

        self.no_error_list = []
        no_error_list_filename = 'clang-check-summary-no-error-list.txt'
        if os.path.exists(no_error_list_filename):
            no_error_list_tmp = []
            with open(no_error_list_filename, 'r') as file_handle:
                no_error_list_tmp = [line.strip() for line in file_handle]
            self.no_error_list = [line for line in no_error_list_tmp if not line.startswith('#')]
        if self.no_error_list:
            logging.info('Allowable warnings: %s', str(self.no_error_list))
        else:
            logging.info('No allowable warnings')
        self.exclusion_filename_list = ['ApexResource.h', 'ApexSFEventLogging.h']

    def is_filename_excluded(self, name):
        """Returns true if the specified pathname refers to an excluded filename"""
        for exclusion_filename in self.exclusion_filename_list:
            if name.endswith(exclusion_filename):
                return True
        return False

    def create_output_byfile(self, summary_byfile):
        """Create the summary by filename file"""
        filename_list = []
        sorted_summary_byfile = {}
        for key, _value in summary_byfile.items():
            if not self.is_filename_excluded(key):
                filename_list.append(key)
        sorted_filenames = sorted(filename_list)
        for filename in sorted_filenames:
            sorted_summary_byfile[filename] = summary_byfile[filename]

        with open(self.output_byfile, 'w') as file_handle:
            print('#', file=file_handle)
            print('# Each record is of the form: <filename>|<issueList>:<grandTotal>', file=file_handle)
            print('# where <issueList> is a comma separated list of issues and the final field', file=file_handle)
            print('# is a grand total of the number of issues.', file=file_handle)
            print('# Each issue in the issue list is an issue name in square brackets, followed by', file=file_handle)
            print('# a colon and the number of occurrences of that issue.', file=file_handle)
            print('#', file=file_handle)
            for key in sorted(sorted_summary_byfile.keys()):
                categories = sorted_summary_byfile[key]
                i = 0
                grand_total = 0
                for key2 in sorted(categories.keys()):
                    value2 = categories[key2]
                    if i == 0:
                        category_info = '{}:{}'.format(key2, value2)
                    else:
                        category_info = '{};{}:{}'.format(category_info, key2, value2)
                    grand_total += value2
                    i = i + 1
                print('{}|{}:{}'.format(key, category_info, grand_total), file=file_handle)
            logging.info('Byfile summary report written to %s.', self.output_byfile)

    def update_summaries(self, category, filename, summary_byfile, summary):
        """Update the summary and summary by file according to the category found"""
        if category in self.catmap:
            category = self.catmap[category]
        if filename in summary_byfile:
            file_summary = summary_byfile[filename]
            if category in file_summary:
                file_summary[category] = file_summary[category] + 1
            else:
                file_summary[category] = 1
        else:
            file_summary = {}
            file_summary[category] = 1
        summary_byfile[filename] = file_summary
        if category in summary:
            summary[category] += 1
        else:
            summary[category] = 1

    def update_summaries_for_mapped_warnings(self, mapped_warnings_by_file, summary_byfile, summary):
        ''' some mapped warning reporting seems a bit variable, so ensure that it is reported only once for each of the various types'''
        for filename in mapped_warnings_by_file:
            lines = mapped_warnings_by_file[filename]
            line_nos = {}
            condensed_lines = []
            for line_category in lines:
                lineno = line_category[2]
                if lineno not in line_nos:
                    line_nos[lineno] = 0
                    condensed_lines.append(line_category)
            for line_category in condensed_lines:
                category = line_category[1]
                self.update_summaries(category, filename, summary_byfile, summary)

    def report(self):
        """Report the league table of issues from lowest to highest occurrences."""
        if self.file is None:
            message = 'File needs to be specified.'
            logging.error(message)
            raise MyException(message)
        if not os.path.exists(self.file):
            message = 'Input file {} not found.'.format(self.file)
            logging.error(message)
            raise MyException(message)
        #
        # Now read from the report.
        try:
            with open(self.file, 'r') as file_handle:
                lines = [line.strip() for line in file_handle]
        except IOError as myexception:
            message = "Couldn't open file {} for reading: {}".format(self.file, myexception.args)
            logging.exception(message)
            raise MyException(message)

        leafnameswithfolder = find_all_leafnames()
        summary = {}
        summary_byfile = {}
        line_number = 0
        line_count = len(lines)
        serious_compilation_error_count = 0
        progress_milestone = 10  # percent

        # override warnings  have to be done file by file, as we only want to report one warning per line
        # for any of the override conditions, as they seem to flip about between what's reported and what's not
        override_warnings_by_file = {}
        nullptr_warnings_by_file = {}
        narrowing_conversions_warnings_by_file = {}
        member_init_warnings_by_file = {}
        anon_namespaces_warnings_by_file = {}
        self_assignment_warnings_by_file = {}
        uppercase_literal_warnings_by_file = {}
        misc_non_private_member_variables_in_classes_by_file = {}
        modernize_avoid_c_arrays_by_file = {}

        for line in lines:
            # It takes quite a while to process the large file so it is nice to
            # give some sort of measure of progress.
            line_number += 1
            percent_complete = 100.0 * line_number / line_count
            if percent_complete >= progress_milestone:
                logging.info('{0:4.2f}% complete so far ({1} of {2} lines) ...'.format(percent_complete, line_number, line_count))
                progress_milestone += 10

            if ' file not found [clang-diagnostic-error]' in line:
                message = 'SERIOUS COMPILATION ERROR DETECTED: {}'.format(line)
                serious_compilation_error_count += 1
                logging.error(message)

            # All notes are ignored.
            if ': note: ' in line:
                continue
            match = re.findall(self.expression, line)
            for item in match:
                if item != '[]':
                    filename = item[0]
                    while '/../' in filename or '\\..\\' in filename or '/./' in filename or '\\.\\' in filename:
                        filename = remove_dot_and_dot_dot_from_path(filename)
                    filename = get_fullname_from_filenameswithfolder(filename, leafnameswithfolder)
                    category = item[2]
                    if category in self.override_categories:
                        # these warnings are done later
                        if filename not in override_warnings_by_file:
                            override_warnings_by_file[filename] = []
                        override_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.nullptr_categories:
                        # these warnings are done later
                        if filename not in nullptr_warnings_by_file:
                            nullptr_warnings_by_file[filename] = []
                        nullptr_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.narrowing_conversions_categories:
                        # these warnings are done later
                        if filename not in narrowing_conversions_warnings_by_file:
                            narrowing_conversions_warnings_by_file[filename] = []
                        narrowing_conversions_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.member_init_categories:
                        # these warnings are done later
                        if filename not in member_init_warnings_by_file:
                            member_init_warnings_by_file[filename] = []
                        member_init_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.anon_namespaces_categories:
                        # these warnings are done later
                        if filename not in anon_namespaces_warnings_by_file:
                            anon_namespaces_warnings_by_file[filename] = []
                        anon_namespaces_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.bugprone_unhandled_self_assignment:
                        # these warnings are done later
                        if filename not in self_assignment_warnings_by_file:
                            self_assignment_warnings_by_file[filename] = []
                        self_assignment_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.readability_uppercase_literal_suffix:
                        # these warnings are done later
                        if filename not in uppercase_literal_warnings_by_file:
                            uppercase_literal_warnings_by_file[filename] = []
                        uppercase_literal_warnings_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.misc_non_private_member_variables_in_classes:
                        # these warnings are done later
                        if filename not in misc_non_private_member_variables_in_classes_by_file:
                            misc_non_private_member_variables_in_classes_by_file[filename] = []
                        misc_non_private_member_variables_in_classes_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    elif category in self.modernize_avoid_c_arrays:
                        # these warnings are done later
                        if filename not in modernize_avoid_c_arrays_by_file:
                            modernize_avoid_c_arrays_by_file[filename] = []
                        modernize_avoid_c_arrays_by_file[filename].append([line, category, item[1]]) # item[1] is the line number
                    else:
                        self.update_summaries(category, filename, summary_byfile, summary)

        # now do the mapped warnings
        self.update_summaries_for_mapped_warnings(override_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(nullptr_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(narrowing_conversions_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(member_init_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(anon_namespaces_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(self_assignment_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(uppercase_literal_warnings_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(misc_non_private_member_variables_in_classes_by_file, summary_byfile, summary)
        self.update_summaries_for_mapped_warnings(modernize_avoid_c_arrays_by_file, summary_byfile, summary)

        if self.output_byfile is not None:
            self.create_output_byfile(summary_byfile)

        sorted_summary = sorted(summary.items(), key=lambda x: (x[1], x[0]))
        message = ''
        with open(self.output, 'w') as file_handle:
            if sorted_summary:
                print('check summary has {} categories and is as follows:-'.format(len(sorted_summary)), file=file_handle)
            else:
                print('check summary is that no checks have been triggered.', file=file_handle)
            for key, value in sorted_summary:
                print('{0:7d} {1}'.format(value, key), file=file_handle)

        logging.info('Summary report written to %s.', self.output)

        if serious_compilation_error_count > 0:
            message = 'There were {} serious compilation errors.'.format(serious_compilation_error_count)
            raise MyException(message)

def main():
    """Main entrypoint for running as standalone script."""
    parser = argparse.ArgumentParser(description=('Analyses a clang-tidy report to show a league table of issues.'))
    parser.add_argument('--file', action='store', required=True, help='The name of the file that contains the report.')
    parser.add_argument('--output', action="store", dest="output", required=True,
                        help='The name of the output file.')
    parser.add_argument('--output_byfile', action="store", dest="output_byfile", required=True,
                        help='The name of the output file summarised by file.')
    args = parser.parse_args()
    sca_summary = ScaSummary(file=args.file, output=args.output, output_byfile=args.output_byfile)
    exit_status = 0
    try:
        sca_summary.report()
    except MyException as ex:
        exit_status = -1
        if str(ex):
            LOGGER.error(str(ex))

    if exit_status == 0:
        LOGGER.info('sca-summary: Report completed.')
    else:
        LOGGER.info('Error: sca-summary report failed.')
    sys.exit(exit_status)

if __name__ == '__main__':
    main()
