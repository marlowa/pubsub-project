"""
Script to perform static code analysis on C++ source using clang-tidy.
This is in preference to using the clang-tidy integration in cmake.
The cmake approach only runs one clang-tidy at the time and this
proves to be too slow. Also it creates one humungous compile_commands.json,
which is one the reasons for performance issues with clang-tidy.
This script runs clang-tidy in batches and generates a fresh compile_commands.json
file for each batch. Members of a given batch are run in parallel.
The way that compile_commands.json is created is very simple: it uses the same compiler
command line options every time. The include directories to search is given by a
config file on the command line. The variation is just in the list of source filenames
and their associated directory.
"""
import sys
import os
import argparse
import logging
import timeit
import math
import concurrent.futures
from subprocess import Popen, PIPE
import platform
import re

# TODO the sca checks completed messages is emitted while there are threads running and producing output.

logging.basicConfig(level=logging.DEBUG, format='%(asctime)s %(levelname)s:: %(message)s')
LOGGER = logging.getLogger(__name__)

# Which files are processed:
#   By default, we process all the C++ sources that we find in the current
#   directory and below. But there is a command line option, --filenames, which
#   takes a file that contains a list of filenames to process. The idea here is
#   that by default we do everything but we can define a job that just does the
#   analysis on a set of files. These might be just a small set or files that
#   have been changed. This provides a way to do incremental analysis during
#   development, rather than waiting for an overnight run.

# Which files are excluded:
#    No matter if the list of files to processes is obtained by walking or
#    reading from the file supplied in the --filenames argument, we have the
#    ability to say that certain files from specified directories are excluded.
#    We do this by a config file that has the list of directory names to skip.

# Formatting the output:
#     By default the output is in whatever form is produced by clang-tidy,
#     but this can be changed. The --output_style=vs option allows the output
#     to be adjusted for Visual Studio. This means that when you click on the
#     clang-tidy in Visual Studio you will be taken to the offending source line.

# Platform considerations:
#     This script is written in python so in theory it should be multi-platform.
#     However, there are some platform considerations that may have received
#     insufficient attention to detail. The main platform that this is expected
#     to run on is linux, where in general, things are simple.
#     With Windows there are some additional concerns that may need attention.
#     Corporate Windows environments have virus scanners that often give rise
#     to spurious file-in-use errors. This means commands may have to be retried.
#     Also the Windows scheduler means that many instances of clang-tidy
#     running concurrently might have to have their priority lowered to
#     avoid crippling the machine.

class MyException(Exception):
    """Application-specific exception so can be distinguished from RuntimeError"""

def get_process_priority(batch_size):
    """Return process priority to use for running clang-tidy"""
    #ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000
    BELOW_NORMAL_PRIORITY_CLASS  = 0x00004000 # pylint: disable=invalid-name,bad-whitespace
    #HIGH_PRIORITY_CLASS         = 0x00000080
    #IDLE_PRIORITY_CLASS         = 0x00000040
    NORMAL_PRIORITY_CLASS        = 0x00000020 # pylint: disable=invalid-name,bad-whitespace
    #REALTIME_PRIORITY_CLASS     = 0x00000100

    pri = 0
    if platform.system() == 'Windows':
        pri = BELOW_NORMAL_PRIORITY_CLASS
        if batch_size == 1:
            pri = NORMAL_PRIORITY_CLASS
    return pri

def setup_delimiters(output_style: str) -> dict:
    """Sets up the delimiters to use in the output of warnings based on the style required"""
    delimiter_dict = {}
    if output_style == 'clang-tidy':
        LOGGER.info('Warning output will be in clang-tidy style')
        delimiter_dict['file_line'] = ':'
        delimiter_dict['line_char'] = ':'
        delimiter_dict['char_type'] = ': '
        delimiter_dict['type_text'] = ': '
    elif output_style == 'vs':
        LOGGER.info('Warning output will be in Visual Studio style')
        delimiter_dict['file_line'] = '('
        delimiter_dict['line_char'] = ','
        delimiter_dict['char_type'] = '): '
        delimiter_dict['type_text'] = ': '
    return delimiter_dict

def replace_using_match(evar_pattern, line, mymatch):
    """Find environment variable and substitute the value in the specified line"""
    key = line[mymatch[0]+2:mymatch[1]-1]
    before = line[0:mymatch[0]]
    after = line[mymatch[1]:]
    value = ''
    try:
        value = os.environ[key]
        matches = [(match.start(), match.end()) for match in re.finditer(evar_pattern, value)]
        if matches:
            message = 'Error: Environment variables are not allowed to refer to other environment variables. %s value is %s' % (key, value)
            raise MyException(message)
        line = '%s%s%s' % (before, value, after)
    except KeyError as key_error:
        message = 'Error: Reference to non-existent environment variable "%s", line "%s"' % (key, line)
        raise MyException(message) from key_error
    return line

def replace_variables(evar_pattern, line):
    """Replace any environment variable references with their values"""
    more_matches = True
    while more_matches:
        matches = [(match.start(), match.end()) for match in re.finditer(evar_pattern, line)]
        if matches:
            line = replace_using_match(evar_pattern, line, matches[0])
        else:
            more_matches = False
    return line

def get_checks_from_ignore_list(filename):
    """Returns the list of checks to ignore, read from config file."""
    checks = ''
    if not os.path.exists(filename):
        LOGGER.error('%s not found.', filename)
        sys.exit(1)
    lines = [line.strip() for line in open(filename, 'r')]
    checklist = []
    for line in lines:
        if line != '' and line[0] != '#':
            if line.startswith('-'):
                line = line[1:]
            checklist.append(line)
    if checklist:
        checks = '-checks=*,'
        i = 0
        for check in checklist:
            i = i + 1
            checks = '{}-{}'.format(checks, check)
            if i < len(checklist):
                checks = '{},'.format(checks)
    return checks

def get_extra_macros(filename):
    """Read a list of macros from a file and form a command line argument list to set them"""
    macros = ''
    with open(filename, 'r') as file:
        lines = [line.strip() for line in file]
        for line in lines:
            line = replace_variables('${\\w*}', line)
            if line and line[0] != '#':
                macros = f'{macros} -D{line}'
    return macros

def read_exclude_files_from_config(filename):
    """Read from a config file a list of filenames to exclude from the analysis"""
    names = []
    with open(filename, 'r') as file:
        lines = [line.strip() for line in file]
        for line in lines:
            line = replace_variables('${\\w*}', line)
            if line and line[0] != '#':
                names.append(line)
    return names

class Sca: # pylint: disable=too-many-instance-attributes
    """Perform an SCA (Static Code Analysis) on C++ files beneath the current directory"""
    def __init__(self, **kwargs):
        self.cwd = os.getcwd()
        self.output_delimiters = setup_delimiters(kwargs.get("output_style"))
        self.number_done = 0
        self.filelist = []
        self.batch_size = kwargs.get("batch_size", -1)
        self.limit = kwargs.get("limit", -1)
        self.pedantic = kwargs.get('pedantic', False)
        self.keep_logs = kwargs.get('keep_logs', False)
        filenames = kwargs.get("filenames", None)
        self.filenames = filenames.name if filenames else None
        exclusion_config = kwargs.get("exclusions", None)
        includes_config = kwargs.get("includes", None)
        ignore_checks_config = kwargs.get("ignore_checks", None)
        extra_macros_config = kwargs.get("extra_macros", None)
        exclude_files_config = kwargs.get("exclude_files", None)
        self.exclusion_dirlist = self.read_exclusion_list(exclusion_config.name) if exclusion_config else []
        self.include_dirlist = self.read_include_list(includes_config.name) if includes_config else []
        self.ignore_checks = get_checks_from_ignore_list(ignore_checks_config.name) if ignore_checks_config else []
        self.extra_macros = get_extra_macros(extra_macros_config.name) if extra_macros_config else ''
        self.exclude_files = read_exclude_files_from_config(exclude_files_config.name) if exclude_files_config else []
        self.get_filelist()
        self.set_batch_size_details()
        self.process_priority = get_process_priority(self.batch_size)

        self.start_time = timeit.default_timer()

    def analyse(self):
        """Perform the analysis"""
        self.process_filelist()

    def set_batch_size_details(self):
        """Set batch size according to how many CPUs we have"""
        if self.batch_size is None or self.batch_size <= 0:
            self.batch_size = os.cpu_count()
        if self.batch_size == 0:
            self.batch_size = 1
        self.max_files_in_batch = 100
        if self.batch_size > 1:
            max_files_for_cpus = self.batch_size * 4
            if max_files_for_cpus > self.max_files_in_batch:
                self.max_files_in_batch = max_files_for_cpus
        self.count = len(self.filelist)
        if self.limit > 0 and self.count > self.limit:
            self.filelist = self.filelist[:self.limit]
            LOGGER.info('There are %s files but due to the limit only %s will be processed.', self.count, self.limit)
            self.count = self.limit
        self.batch_count = int(math.ceil(self.count / self.batch_size))
        LOGGER.info('%d files to process, batch size is %d, max files in a batch is %d, batch count = %d',
                    self.count, self.batch_size, self.max_files_in_batch, self.batch_count)

    def get_fullname(self, filename):
        """Return fully qualified filename given relative name."""
        full_filename = os.path.join(self.cwd, filename)
        full_filename = full_filename.replace('\\', '/')
        if full_filename.startswith('./'):
            full_filename = full_filename[2:]
        while '/./' in full_filename:
            full_filename = full_filename.replace('/./', '/')
        return full_filename

    def read_exclusion_list(self, exclusion_config):
        """Read from a config file a list of module names to exclude from the analysis"""
        return self.read_names_from_config(exclusion_config)

    def read_include_list(self, includes_config):
        """Read from a config file a list of pathames for include directories.
           Each line is the name of an include directory, optionally followed by the word 'system',
           to indicate a system directory. Headers from such directories are handled differently
           during compilation. Any warnings the compiler might emit are suppressed.
        """
        includes = []
        with open(includes_config, 'r') as file:
            lines = [line.strip() for line in file]
            for line in lines:
                line = replace_variables('${\\w*}', line)
                if line and line[0] != '#':
                    tokens = line.split(' ')
                    inc = tokens[0]
                    if not inc.startswith("/") and os.path.exists(inc):
                        inc = self.cwd + "/" + inc
                    ftype = 'system' if len(tokens) == 2 and tokens[1] == 'system' else ''
                    includes.append([inc, ftype])
        return includes

    def read_names_from_config(self, filename):
        """Read from a config file a list of names/tokens"""
        names = []
        with open(filename, 'r') as file:
            lines = [line.strip() for line in file]
            for line in lines:
                line = replace_variables('${\\w*}', line)
                if line and line[0] != '#':
                    if line not in self.filelist:
                        names.append(line)
        return names

    def is_directory_excluded(self, name):
        """Returns true if the specified pathname refers to an excluded directory"""
        omit = False
        for exclusion_dir in self.exclusion_dirlist:
            edir = '{}/'.format(exclusion_dir) if '/' in exclusion_dir else '/{}/'.format(exclusion_dir)
            if edir in name:
                LOGGER.info('Skipping file %s because it refers to excluded directory %s', name, exclusion_dir)
                omit = True
                continue
        return omit

    def check_name_excluded(self, root, name, filelist):
        """adds non-excluded name to the filelist."""
        if self.is_directory_excluded(name):
            return
        if len(root) > 1 and root[0] == '.' and (root[1] == '\\' or root[1] == '/'):
            root = root[2:]
        filelist.append(name)

    def get_filelist(self):
        """Get the list of files to process, either by reading from filenames file or by walking the tree
           Any filenames that appear in the exclude_files list are not included.
        """
        if self.filenames is None:
            self.filelist = self.build_list_of_files_to_process()
            return
        if not os.path.exists(self.filenames):
            raise MyException(f'The file {self.filenames} does not exist.')
        self.filelist = []
        with open(self.filenames, 'r') as file:
            lines = [line.strip() for line in file]
            for line in lines:
                if line != '' and line[0] != '#':
                    if line.startswith('./') or line.startswith('.\\'):
                        line = line[2:]
                        line = replace_variables('${\\w*}', line)
                    if line not in self.filelist:
                        if not os.path.exists(line):
                            LOGGER.info('File %s not found. Ignoring', line)
                        else:
                            if line in self.exclude_files:
                                LOGGER.info("File %s excluded.", line)
                            else:
                                self.filelist.append(line)

    def build_list_of_files_to_process(self):
        """Build a list of filenames to process by walking the tree from the current directory and including every cpp files unless it comes from a directory that is to be excluded"""
        filelist = []
        for root, _dirs, files in os.walk("."):
            for filename in files:
                suffix = os.path.splitext(filename)[1][1:]
                if suffix in ['cpp', 'cc']:
                    name = os.path.join(root, filename)
                    name = name.replace('\\', '/')  # The clang tools prefer forward slashes.
                    self.check_name_excluded(root, name, filelist)
        filelist.sort()
        return filelist

    def process_filelist(self):
        """Process all the files in the list, doing a clang-tidy for each one in parallel in batches"""
        pool_size = 1 if self.batch_size == 0 else self.batch_size
        for batch in range(self.batch_count):
            self.process_batch(batch, pool_size)

    # TODO need to think about clang-tidy cleanup after keyboard interrupt.
    def process_batch(self, batch, pool_size):
        """Process a batch by writing the compile_commands.json then running the clang-tidy commands in parallel"""
        count = self.batch_size
        if ((batch+1) * self.batch_size) > self.count:
            count = self.count % self.batch_size
        self.create_json_file(count, batch)

        with concurrent.futures.ThreadPoolExecutor(max_workers=pool_size) as executor:
            results = [executor.submit(self.call_clang_tidy, item_number, batch) for item_number in range(count)]

        for my_future in concurrent.futures.as_completed(results):
            filename, index, output, _err, returncode, exception_text = my_future.result()

            output = self.tidy_up_clang_output(filename, output)

            # Note that the lines are not output by the logger but sent to the console directly.
            # We don't want the output adorned by the logger because that would mess up any
            # formatting done for IDEs. It would also mess up interpretation by the summary script.
            for line in output:
                print(f'{line}')
            if returncode == 0:
                LOGGER.info('Finished clang-tidy ok for index %d: %s', index, filename)
            else:
                LOGGER.info('Finished clang-tidy with error, index %d file: %s', index, filename)
                if exception_text:
                    LOGGER.info("%s", exception_text)

        self.number_done += count
        self.report_batch_progress()

    def run_command_and_get_output(self, command, item_number):
        """Run command and return output even when command gives an exception"""
        output = None
        exception_text = ''
        returncode = 0

        # Something very weird was discovered while working on this script:
        # Capturing output using popen's stdout pipe seems to mess up the newlines.
        # Hence we redirect to a file, then read from the file.

        log_filename = os.path.join(self.cwd, f'clang-out-{item_number}.txt')
        command = f'{command} >{log_filename} 2>&1'
        output = ''
        try:
            invocation = Popen(command, shell=True, stdin=PIPE, stdout=PIPE, stderr=PIPE, creationflags=self.process_priority)
            _output_pipe, _err = invocation.communicate()
            returncode = invocation.returncode
        except Exception as ex: # pylint: disable=broad-except
            returncode = -1
            exception_text = str(ex)

        if os.path.exists(log_filename):
            with open(log_filename, 'r') as file:
                output = [line.strip() for line in file]
            if not self.keep_logs:
                os.remove(log_filename)
        return output, "", returncode, exception_text

    def call_clang_tidy(self, item_number, batch):
        """Run clang-tidy on the file specified by the given index into the list."""
        index = item_number + (batch * self.batch_size)
        filename = self.filelist[index]
        full_filename = self.get_fullname(filename)
        LOGGER.info('Starting clang-tidy for batch %4d item %4d index %4d: %s', batch, item_number, index, full_filename)

        check_arguments = self.get_check_arguments()
        call_command = f'clang-tidy --extra-arg=-ferror-limit=100000 {check_arguments} {full_filename}'
        LOGGER.info("clang-tidy command: %s", call_command)
        output, err, returncode, exception_text = self.run_command_and_get_output(call_command, index)
        return filename, index, output, err, returncode, exception_text

    def tidy_up_clang_output(self, filename, output):
        """process clang-tidy output, ensuring that output is with prescribed format and warnings have
           the proper pathname to the file. When we say "proper" filename we mean the path relative to
           the current directory. Sometimes clang-tidy uses that name, sometimes it uses the leaf name
           and sometimes it uses the full pathname. We need the file to be named consistently otherwise
           the summary by filename will be wrong.
        """
        new_output = []
        leafname = os.path.basename(filename)
        for line in output:
            if re.search('Suppressed [0-9]* warnings', line): # skip Suppressed [0-9]* warnings
                continue
            if re.search('/usr/include/', line):
                continue
            if not f'{leafname}:' in line:
                new_output.append(line) # we still want the output to contain the details.
                continue
            match = re.search(f'^(.*{leafname}):[0-9]*:[0-9]*: ', line)
            if match:
                found_filename = match.group(1)
                line = line.replace(found_filename, filename)
            else:
                print(f'ERROR: no match: ERROR: leafname {leafname} found using "in" but RE does not find it, line: {line}.')
                sys.exit(1)

            # TODO This is where we need to cater for different delimiters for source line number and column number depending on output format e.g Visual Studio Code.
            result = re.search(r'^(?P<cpp_filename>.+):(?P<line_number>\d+):(?P<character_number>\d+): (?P<warning_type>(warning|note|error)): (?P<warning_text>.*)', line)
            result = None
            if result is not None:
                replacement_filename = filename if leafname in result.group('cpp_filename') else result.group('cpp_filename')
                new_line = '{filename}{delim1}{line_number}{delim2}{char_num}{delim3}{warning_type}{delim4}{warning_text}\n'.format(
                    filename=replacement_filename, delim1=self.output_delimiters['file_line'], line_number=result.group('line_number'),
                    delim2=self.output_delimiters['line_char'], char_num=result.group('character_number'),
                    delim3=self.output_delimiters['char_type'], warning_type=result.group('warning_type'),
                    delim4=self.output_delimiters['type_text'], warning_text=result.group('warning_text'))
                new_output.append(new_line)
            else:
                new_output.append(line)
        return new_output

    def create_json_file(self, count, batch):
        """Write compile_command.json will all the file and compiler details for this batch"""
        try:
            json_file_handle = open('compile_commands.json', 'w')
        except Exception as ex: # pylint: disable=broad-except
            raise MyException("Failed to open compile_commands.json for writing: " + str(ex.args))
        print('[', file=json_file_handle)
        for item in range(count):
            index = item + (batch * self.batch_size)
            filename = self.filelist[item]
            full_filename = self.get_fullname(filename)
            directory_name = os.path.dirname(full_filename)
            leafname = os.path.basename(full_filename)
            print('  {', file=json_file_handle)
            print('    "directory": "{}",'.format(directory_name), file=json_file_handle)
            print('{}'.format(self.json_command_for_gcc(directory_name, leafname)), file=json_file_handle)
            print('    "file": "{}"'.format(full_filename), file=json_file_handle)
            if (item+1) == count:
                print('  }', file=json_file_handle)
            else:
                print('  },', file=json_file_handle)
        print(']', file=json_file_handle)
        json_file_handle.close()

    def json_command_for_gcc(self, dirname, leafname):
        """Return the compiler command plus args for the clang-tidy json file for unix"""
        includes = ''
        for include_info in self.include_dirlist:
            if include_info[1] == 'system':
                includes += f' -isystem {include_info[0]}'
            else:
                includes += f' -I{include_info[0]}'

        pedantic_option = '-pedantic' if self.pedantic else ''

        return ('    "command": "/usr/local/gcc/bin/g++ -c -DLINUX {macros_option} '
                '-Wall {pedantic} -std=c++17 -Wno-long-long -Wdeprecated-declarations '
                ' -I{first_dirname} {includes}'
                ' {leafname}",'.format(pedantic=pedantic_option, macros_option=self.extra_macros,
                                       first_dirname=dirname, includes=includes, leafname=leafname))

    def report_batch_progress(self):
        """Report how many files are done and average speed per file."""
        time_so_far = timeit.default_timer()
        percent_complete = 100.0 * self.number_done / self.count
        speed_per_file = (time_so_far - self.start_time) / self.number_done
        LOGGER.info(('There are {0:5d} files processed so far ({1:6.2f}%) with {2:5d} remaining. '
                     'Average speed = {3:8.3f} seconds per file.').format(self.number_done, percent_complete, self.count - self.number_done, speed_per_file))

    def get_check_arguments(self):
        """Return the checks arguments which is all checks, optionally followed by checks to ignore"""
        check_arguments = '-checks=*'
        if self.ignore_checks:
            check_arguments = self.ignore_checks
        return check_arguments

def main():
    """Main entrypoint for running as standalone script."""
    parser = argparse.ArgumentParser(description=('Checks C++ source code using clang tools. ' +
                                                  'The static analysis is written to the console.'))
    parser.add_argument('--batch_size', action="store", dest="batch_size", type=int,
                        help='The maximum number of jobs that are allowed to be run concurrently. ' +
                        'None, 0 or negative will default to numcpus')
    parser.add_argument('--limit', action="store", dest="limit", type=int, default=-1,
                        help='The maximum number of files to process.')
    parser.add_argument('--filenames', action="store", dest="filenames", type=argparse.FileType('r'),
                        help='File that contains a list of filenames to check.')
    parser.add_argument('--exclusions', action="store", dest="exclusions", type=argparse.FileType('r'),
                        help='A config file that contains a list of directories to exclude from the analysis')
    parser.add_argument('--exclude_files', action="store", dest="exclude_files", type=argparse.FileType('r'),
                        help='A config file that contains a list of filenames to exclude from the analysis')
    parser.add_argument('--ignore_checks', action="store", dest="ignore_checks", type=argparse.FileType('r'),
                        help='A config file that contains a list of checks to ignore during the analysis')
    parser.add_argument('--includes', action='store', dest='includes', type=argparse.FileType('r'),
                        help='File that contains a list of include directories to use')
    parser.add_argument('--extra-macros', action="store", dest="extra_macros", type=argparse.FileType('r'),
                        help='A config file that contains a list of extra macros to add to the compiler command line')
    parser.add_argument('--pedantic', action='store', dest='pedantic',
                        help='Add the -pedantic flag to the compiler')
    parser.add_argument('--keep_logs', action='store_true', dest='keep_logs',
                        help='Keep individual logs from clang-tidy runs (normally tidied up after use)')
    parser.add_argument('--output_style', action='store', default='clang-tidy', choices=['clang-tidy', 'vs'],
                        help='Output style of warning messages produced by clang.')

    args = parser.parse_args()
    exit_status = 0
    try:
        analyser = Sca(batch_size=args.batch_size,
                       limit=args.limit,
                       filenames=args.filenames,
                       exclusions=args.exclusions,
                       includes=args.includes,
                       ignore_checks=args.ignore_checks,
                       extra_macros=args.extra_macros,
                       exclude_files=args.exclude_files,
                       keep_logs=args.keep_logs,
                       output_style=args.output_style)
        analyser.analyse()
    except MyException as ex:
        exit_status = -1
        if str(ex):
            LOGGER.error(str(ex))

    if exit_status == 0:
        LOGGER.info('sca: All checks completed.')
    else:
        LOGGER.info('Error: sca checking failed.')
    sys.exit(exit_status)

if __name__ == '__main__':
    main()
