# Copyright (c) 2014-2016, The Regents of the University of California.
# Copyright (c) 2016-2017, Nefeli Networks, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# * Neither the names of the copyright holders nor the names of their
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

import sys
import os
from operator import itemgetter


class ColorizedOutput(object):  # for pretty printing

    def __init__(self, orig_out, color):
        self.orig_out = orig_out
        self.color = color

    def __getattr__(self, attr):
        def_color = '\033[0;0m'  # resets all terminal attributes

        if attr == 'write':
            return lambda x: self.orig_out.write(self.color + x + def_color)
        else:
            return getattr(self.orig_out, attr)


class CLI(object):

    class CommandError(Exception):  # general command errors
        pass

    class HandledError(Exception):
        pass

    class InvalidCommandError(Exception):
        pass

    # variable binding errors
    class BindError(Exception):
        pass

    # some internal logic errors that might be your (or my) fault
    class InternalError(Exception):
        pass

    def __init__(self, cmdlist, fin=sys.stdin, fout=sys.stdout, ferr=None,  
             interactive=None, history_file=None):  
        """  
        Initialize the CLI with proper I/O streams, history handling, and interactive mode.  
        
        Args:  
            cmdlist: List of available commands  
            fin: Input stream (default: stdin)  
            fout: Output stream (default: stdout)  
            ferr: Error stream (default: auto-detected with color support)  
            interactive: Force interactive mode (default: auto-detect)  
            history_file: Command history file path (default: ~/.bess_history)  
        """  
        self.cmdlist = cmdlist  
        self._setup_basic_streams(fin, fout)  
        self._setup_history_file(history_file)  
        self._setup_error_output(ferr)  
        self._setup_interactive_mode(fin, fout, interactive)  
        
        # Start interactive mode if enabled  
        if self.interactive:  
            self.go_interactive()  
    
    def _setup_basic_streams(self, fin, fout):  
        """Set up basic I/O streams and initialize command state."""  
        self.fin = fin  
        self.fout = fout  
        self.last_cmd = ''  
        self.rl = None  
    
    def _setup_history_file(self, history_file):  
        """  
        Configure command history file path.  
        
        Uses ~/.bess_history by default, but gracefully handles errors  
        if the home directory cannot be accessed.  
        """  
        if history_file is None:  
            try:  
                self.history_file = os.path.expanduser('~/.bess_history')  
            except:  
                # Fallback if home directory is not accessible  
                self.history_file = None  
        else:  
            self.history_file = history_file  
    
    def _setup_error_output(self, ferr):  
        """  
        Configure error output with optional colorization.  
        
        Automatically detects terminal capabilities and enables red color  
        for error messages when appropriate.  
        """  
        if ferr is None:  
            # Auto-detect color support based on terminal type and TTY status  
            if os.environ.get('TERM') != 'dumb' and sys.stderr.isatty():  
                # Use dark red color for error messages in capable terminals  
                self.ferr = ColorizedOutput(sys.stderr, '\033[31m')  
            else:  
                # Fallback to plain stderr for dumb terminals or non-TTY output  
                self.ferr = sys.stderr  
        else:  
            # Use explicitly provided error stream  
            self.ferr = ferr  
    
    def _setup_interactive_mode(self, fin, fout, interactive):  
        """  
        Determine if CLI should run in interactive mode.  
        
        Interactive mode is enabled by default when both input and output  
        are connected to a terminal (TTY). Can be explicitly forced on/off.  
        """  
        if interactive is None:  
            # Auto-detect based on TTY status of input/output streams  
            self.interactive = fin.isatty() and fout.isatty()  
        else:  
            # Use explicitly specified interactive mode setting  
            self.interactive = interactive

    def err(self, msg):
        self.ferr.write('*** Error: %s\n' % msg)
        if not self.interactive:
            self.stop_loop = True

    # If not a variable, simply return None
    # Otherwise, return (var_type, desc, candidates):
    #    var_type can be: 'int', 'str', 'list'(list of strings), 'map'
    #    candidates is a list of string values.
    def get_var_attrs(self, var_token, partial_word):
        return None

    # Return (head, tail)
    #   head: consumed string portion
    #   tail: the rest of input line
    # You can assume that 'line == head + tail'
    def split_var(self, var_type, line):
        if var_type == 'keyword':
            pos = line.find(' ')
            if pos == -1:
                return line, ''
            else:
                return line[:pos], line[pos:]

        raise self.InternalError('type "%s" is undefined' % var_type)

    # Return (mapped_value, tail)
    #   mapped_value: Python value/object from the consumed token(s)
    #   tail: the rest of input line
    def bind_var(self, var_type, line):
        if var_type == 'keyword':
            return None, self.split_var(var_type, line)[1]

        raise self.InternalError('type "%s" is undefined' % var_type)

    # Compare a command with a user-typed line.
    # It returns (match_type, candidates, syntax_token, score).
    # match_type can be:
    #  - 'full': all tokens in syntax was consumed
    #  - 'partial': prefix matched
    #  - 'nonmatch': not a match
    # candidates is a list of suggested strings to be added as the last token.
    # syntax_token is where the user input is currently on, if any.
    # score is the number of matched keywords
    # exact_score is the number of "exactly" matched keywords
    def match(self, syntax, line):  
        """  
        Match a command syntax pattern against user input line.  
        
        This function implements the core command matching logic for CLI tab completion.  
        It analyzes how well the user's input matches a command syntax and returns  
        appropriate completion candidates.  
        
        Args:  
            syntax: Command syntax pattern (e.g., "add module MODULENAME")  
            line: User input line to match against  
            
        Returns:  
            tuple: (match_type, candidates, syntax_token, score, exact_score)  
                match_type: 'full', 'partial', or 'nonmatch'  
                candidates: List of completion suggestions  
                syntax_token: Current token position in syntax  
                score: Number of matched keywords  
                exact_score: Number of exactly matched keywords  
        """  
        # Initialize matching state  
        match_state = self._initialize_match_state(line)  
        syntax_tokens = syntax.split()  
        
        # Process each syntax token against the input  
        for i, syntax_token in enumerate(syntax_tokens):  
            word_info = self._get_current_word_info(match_state['remainder'])  
            var_attrs = self._get_variable_attributes(syntax_token, word_info['word'])  
            
            # Handle case where no more input remains  
            if match_state['remainder'].strip() == '':  
                return self._handle_empty_remainder(  
                    i, syntax_token, syntax_tokens, var_attrs, match_state  
                )  
            
            # Process the current token  
            token_result = self._process_token_match(  
                syntax_token, var_attrs, match_state  
            )  
            
            # Update match state based on token processing  
            match_state.update(token_result['state_update'])  
            
            # Handle keyword-specific matching logic  
            if var_attrs['type'] == 'keyword':  
                keyword_result = self._handle_keyword_match(  
                    syntax_token, token_result['token'], match_state  
                )  
                if keyword_result['early_return']:  
                    return keyword_result['return_value']  
                match_state.update(keyword_result['state_update'])  
            else:  
                # Handle variable-type matching  
                var_result = self._handle_variable_match(  
                    var_attrs['candidates'], token_result['token'], match_state  
                )  
                match_state['candidates'] = var_result['candidates']  
        
        # Finalize the match result after processing all tokens  
        return self._finalize_match_result(match_state, syntax_tokens[-1])  
    
    def _initialize_match_state(self, line):  
        """  
        Initialize the matching state with default values.  
        
        Args:  
            line: User input line  
            
        Returns:  
            dict: Initial matching state  
        """  
        return {  
            'candidates': [],  
            'remainder': line,  
            'score': 0,  
            'exact_score': 0,  
            'new_token': (line != '' and line[-1] == ' ')  
        }  
    
    def _get_current_word_info(self, remainder):  
        """  
        Extract the current word from the remaining input.  
        
        Args:  
            remainder: Remaining input to process  
            
        Returns:  
            dict: Word information with 'word' key  
        """  
        if remainder.split():  
            return {'word': remainder.split()[0]}  
        return {'word': ''}  
    
    def _get_variable_attributes(self, syntax_token, line_word):  
        """  
        Get variable attributes for a syntax token.  
        
        Args:  
            syntax_token: Token from command syntax  
            line_word: Current word from user input  
            
        Returns:  
            dict: Variable attributes with 'type', 'desc', 'candidates' keys  
        """  
        attrs = self.get_var_attrs(syntax_token, line_word)  
        if attrs:  
            return {  
                'type': attrs[0],  
                'desc': attrs[1],   
                'candidates': attrs[2]  
            }  
        return {'type': 'keyword', 'desc': '', 'candidates': []}  
    
    def _handle_empty_remainder(self, i, syntax_token, syntax_tokens, var_attrs, match_state):  
        """  
        Handle matching when no more input remains.  
        
        This occurs when the user has entered less than the full command.  
        We generate completion candidates based on what could come next.  
        
        Args:  
            i: Current token index  
            syntax_token: Current syntax token  
            syntax_tokens: List of all syntax tokens  
            var_attrs: Variable attributes for current token  
            match_state: Current matching state  
            
        Returns:  
            tuple: Match result tuple  
        """  
        if match_state['new_token']:  
            # Clear candidates if this is the first token or previous wasn't ellipsis  
            if i == 0 or '...' not in syntax_tokens[i - 1]:  
                match_state['candidates'] = []  
            
            # Add variable candidates and syntax token if it's a keyword  
            match_state['candidates'].extend(var_attrs['candidates'])  
            if var_attrs['type'] == 'keyword':  
                match_state['candidates'].append(syntax_token)  
            
            # Check if current token is skippable (optional)  
            if syntax_token[0] == '[':  
                return 'full', match_state['candidates'], syntax_token, match_state['score'], match_state['exact_score']  
            
            return 'partial', match_state['candidates'], syntax_token, match_state['score'], match_state['exact_score']  
        
        # Return partial match with previous token  
        return 'partial', match_state['candidates'], syntax_tokens[max(0, i - 1)], match_state['score'], match_state['exact_score']  
    
    def _process_token_match(self, syntax_token, var_attrs, match_state):  
        """  
        Process matching of a single token against the input.  
        
        Args:  
            syntax_token: Current syntax token to match  
            var_attrs: Variable attributes for the token  
            match_state: Current matching state  
            
        Returns:  
            dict: Token processing result with token and state updates  
        """  
        token, remainder = self.split_var(var_attrs['type'], match_state['remainder'])  
        
        return {  
            'token': token,  
            'state_update': {  
                'remainder': remainder.lstrip()  
            }  
        }  
    
    def _handle_keyword_match(self, syntax_token, token, match_state):  
        """  
        Handle matching for keyword tokens.  
        
        Keywords must match exactly or be a prefix of the syntax token.  
        
        Args:  
            syntax_token: Expected keyword from syntax  
            token: Actual token from user input  
            match_state: Current matching state  
            
        Returns:  
            dict: Keyword match result with potential early return  
        """  
        if syntax_token == token:  
            # Exact match - update candidates based on new token status  
            if match_state['new_token']:  
                candidates = []  
            else:  
                candidates = [syntax_token]  
            
            state_update = {  
                'candidates': candidates,  
                'score': match_state['score'] + 1  
            }  
            
            # Increment exact score for perfect matches  
            if syntax_token.strip() == token:  
                state_update['exact_score'] = match_state['exact_score'] + 1  
            else:  
                state_update['exact_score'] = match_state['exact_score']  
                
            return {'early_return': False, 'state_update': state_update}  
        else:  
            # Partial match - check if token is prefix of syntax token  
            if not syntax_token.startswith(token):  
                return {  
                    'early_return': True,  
                    'return_value': ('nonmatch', [], '', match_state['score'],   
                                match_state['exact_score'])  
                }  
            
            return {  
                'early_return': False,  
                'state_update': {'candidates': [syntax_token]}  
            }  
    
    def _handle_variable_match(self, var_candidates, token, match_state):  
        """  
        Handle matching for variable-type tokens.  
        
        Variables can take multiple values, so we filter candidates based on  
        the current token prefix.  
        
        Args:  
            var_candidates: List of possible variable values  
            token: Current token from user input  
            match_state: Current matching state  
            
        Returns:  
            dict: Variable match result with updated candidates  
        """  
        if match_state['new_token']:  
            # New token - show all variable candidates  
            return {'candidates': var_candidates}  
        else:  
            # Existing token - filter candidates by prefix  
            candidates = []  
            if token.split():  
                last_word = token.split()[-1]  
                for var in var_candidates:  
                    if var.startswith(last_word):  
                        candidates.append(var)  
            return {'candidates': candidates}  
    
    def _finalize_match_result(self, match_state, last_syntax_token):  
        """  
        Finalize the match result after processing all tokens.  
        
        Args:  
            match_state: Final matching state  
            last_syntax_token: The last processed syntax token  
            
        Returns:  
            tuple: Final match result  
        """  
        if match_state['remainder'].strip() == '':  
            # No remaining input - check for ellipsis or return full match  
            if '...' in last_syntax_token:  
                return 'full', match_state['candidates'], last_syntax_token, match_state['score'], match_state['exact_score']  
            
            if match_state['new_token']:  
                return 'full', ['\n'], '', match_state['score'], match_state['exact_score']  
            
            return 'full', match_state['candidates'], last_syntax_token, match_state['score'], match_state['exact_score']  
        
        # Remaining input indicates non-match  
        return 'nonmatch', [], '', match_state['score'], match_state['exact_score']

    # filter is one of 'full', 'partial', 'nonmatch'
    def list_matched(self, line, filter):
        matched_list = []

        for cmd in self.cmdlist:
            syntax = cmd[0]
            match_type, _, _, score, exact_score = self.match(syntax, line)

            if match_type == filter:
                matched_list.append((cmd, score, exact_score))

        if len(matched_list) == 0:
            return [], []

        max_score = max([x[1] for x in matched_list])

        ret = [m[0] for m in matched_list if m[1] == max_score]
        ret_low = [m[0] for m in matched_list if m[1] != max_score]

        # Find exact matches without ambiguity
        if filter == 'full' and len(ret) > 1:
            full_matches = [m for m in matched_list if m[1] == max_score]
            # sorted by exact score
            full_matches.sort(key=itemgetter(2), reverse=True)
            if full_matches[0][2] != full_matches[1][2]:
                return [full_matches[0][0]], []

        return ret, ret_low

    def _do_complete(self, line, partial_word):  
        """  
        Handle tab completion for CLI commands.  
        
        This function generates completion candidates based on the current input  
        line and partial word, then either returns candidates for tab completion  
        or displays help information if multiple matches exist.  
        
        Args:  
            line: Current command line input  
            partial_word: The word being completed  
            
        Returns:  
            list: Completion candidates (empty if help was displayed)  
        """  
        # Collect all possible command matches and candidates  
        completion_state = self._collect_command_matches(line, partial_word)  
        
        # Try to find common prefix for auto-completion  
        common_prefix_result = self._find_common_prefix(  
            completion_state['candidates'], partial_word  
        )  
        
        if common_prefix_result['should_return']:  
            return common_prefix_result['candidates']  
        
        # Display help information for all possible commands  
        self._display_completion_help(completion_state['possible_cmds'], partial_word)  
        
        return []  
    
    def _collect_command_matches(self, line, partial_word):  
        """  
        Collect all matching commands and their completion candidates.  
        
        Args:  
            line: Current command line input  
            partial_word: The word being completed  
            
        Returns:  
            dict: State containing possible_cmds, candidates, and num_full_matches  
        """  
        possible_cmds = []  
        candidates = []  
        num_full_matches = 0  
        
        for cmd in self.cmdlist:  
            syntax = cmd[0]  
            match_type, sub_candidates, syntax_token, _, _ = self.match(syntax, line)  
            
            if match_type in ['full', 'partial']:  
                possible_cmds.append((cmd, match_type, syntax_token))  
                
                if match_type == 'full':  
                    num_full_matches += 1  
                
                # Process and filter candidates based on partial word  
                filtered_candidates = self._filter_candidates(  
                    sub_candidates, partial_word  
                )  
                candidates.extend(filtered_candidates)  
        
        # Remove duplicates and sort candidates  
        candidates = sorted(list(set(candidates)))  
        
        return {  
            'possible_cmds': possible_cmds,  
            'candidates': candidates,  
            'num_full_matches': num_full_matches  
        }  
    
    def _filter_candidates(self, sub_candidates, partial_word):  
        """  
        Filter and format completion candidates based on partial word.  
        
        Args:  
            sub_candidates: List of potential candidates from matching  
            partial_word: The word being completed  
            
        Returns:  
            list: Filtered and formatted candidates  
        """  
        filtered = []  
        
        for candidate in sub_candidates:  
            if candidate.startswith(partial_word):  
                # Add space to candidates that aren't directories or newlines  
                if not candidate.endswith('/') and candidate != '\n':  
                    candidate += ' '  
                filtered.append(candidate)  
        
        return filtered  
    
    def _find_common_prefix(self, candidates, partial_word):  
        """  
        Find the longest common prefix among all candidates.  
        
        If a common prefix exists and extends beyond the current partial word,  
        return the candidates for tab completion. Otherwise, indicate that  
        help should be displayed.  
        
        Args:  
            candidates: List of completion candidates  
            partial_word: The word being completed  
            
        Returns:  
            dict: Result indicating whether to return candidates and the candidates list  
        """  
        if not candidates:  
            return {'should_return': False, 'candidates': []}  
        
        # Find common prefix using min/max string comparison  
        s_min = candidates[0]  
        s_max = candidates[-1]  
        
        for i, c in enumerate(s_min):  
            if i >= len(s_max) or c != s_max[i]:  
                common_prefix = s_min[:i]  
                break  
        else:  
            common_prefix = s_min  
        
        # Check if we can auto-complete based on common prefix  
        if (common_prefix and len(partial_word) < len(common_prefix) and  
                partial_word == common_prefix[:len(partial_word)]):  
            # Return non-empty candidates for tab completion  
            filtered_candidates = [c for c in candidates if c.strip() != '']  
            return {  
                'should_return': True,  
                'candidates': filtered_candidates if filtered_candidates else []  
            }  
        
        return {'should_return': False, 'candidates': []}  
    
    def _display_completion_help(self, possible_cmds, partial_word):  
        """  
        Display help information for all possible commands.  
        
        Shows command syntax, descriptions, and available variable candidates  
        for each matching command.  
        
        Args:  
            possible_cmds: List of possible command matches  
            partial_word: The word being completed  
        """  
        buf = []  
        
        for cmd, match_type, syntax_token in possible_cmds:  
            syntax, desc, _ = cmd  
            
            # Format command line based on match type  
            if match_type == 'full' and len(possible_cmds) == 1:  
                buf.append('  %-50s %s\n' % (syntax + ' <enter>', desc))  
            else:  
                buf.append('  %-50s %s\n' % (syntax, desc))  
            
            # Show variable information if current token is a variable  
            if syntax_token:  
                var_info = self._get_variable_info(syntax_token, partial_word)  
                buf.extend(var_info)  
        
        if buf:  
            self._write_help_output(buf)  
    
    def _get_variable_info(self, syntax_token, partial_word):  
        """  
        Get formatted information about a variable token.  
        
        Args:  
            syntax_token: The current syntax token  
            partial_word: The word being completed  
            
        Returns:  
            list: Formatted lines with variable information  
        """  
        buf = []  
        
        attrs = self.get_var_attrs(syntax_token, partial_word)  
        if attrs:  
            var_type, var_desc, var_candidates = attrs  
            buf.append('    %s (%s): %s\n' % (syntax_token, var_type, var_desc))  
            
            # Show variable candidates that match the partial word  
            for var in var_candidates:  
                if var.startswith(partial_word):  
                    buf.append('      %s\n' % var)  
        
        return buf  
    
    def _write_help_output(self, buf):  
        """  
        Write the help output to the terminal and restore prompt.  
        
        Args:  
            buf: Buffer containing formatted help text  
        """  
        # Get current line for prompt restoration  
        current_line = self.rl.get_line_buffer() if self.rl else ''  
        
        self.fout.write('\n')  
        self.fout.write(''.join(buf))  
        self.fout.write('%s%s' % (self.get_prompt(), current_line))  
        self.fout.flush()

    def complete(self, partial_word, state):
        if state == 0:
            line = self.rl.get_line_buffer()

            # We currently support auto completion only at the EOL
            if len(line) != self.rl.get_endidx():
                return None

            # All exceptions happening here is ignored by the caller,
            # so we add our exception handler for debugging
            try:
                self.candidates = self._do_complete(line, partial_word)
            except BaseException as e:
                import traceback
                traceback.print_exc()
                sys.exit(1)

        try:
            return self.candidates[state]
        except IndexError:
            return None

    def complete_dummy(self, partial_word, state):
        return None

    def get_prompt(self):
        return '> '

    def find_cmd(self, line):
        # return commands being matched with every token
        matched, matched_low = self.list_matched(line, 'full')
        line_stripped = line.strip()

        if len(matched) == 1:
            return matched[0]

        elif len(matched) >= 2:
            self.err('Ambiguous command "%s". Candidates:' % line_stripped)
            for cmd, desc, _ in matched + matched_low:
                self.ferr.write('  %-50s%s\n' % (cmd, desc))

        elif len(matched) == 0:
            matched, matched_low = self.list_matched(line, 'partial')
            if len(matched) > 0:
                self.err('Incomplete command "%s". Candidates:' %
                         line_stripped)
                for cmd, desc, _ in matched + matched_low:
                    self.ferr.write('  %-50s%s\n' % (cmd, desc))
            else:
                self.err('Unknown command "%s".' % line_stripped)

        raise self.InvalidCommandError()

    def get_default_args(self):
        return []

    def bind_args(self, cmd, line):
        syntax, desc, func = cmd
        remainder = line
        args = []

        for i, syntax_token in enumerate(syntax.split()):
            if remainder.strip() == '':
                if syntax_token[0] == '[':
                    args.append(None)
                    continue

                raise self.InternalError('Partial match on "%s"? line: "%s"' %
                                         (syntax, line))

            attrs = self.get_var_attrs(syntax_token, remainder.split()[0])
            if attrs:
                var_type = attrs[0]
            else:
                var_type = 'keyword'

            val, remainder = self.bind_var(var_type, remainder)

            if var_type != 'keyword':
                args.append(val)

            remainder = remainder.lstrip()

        args = self.get_default_args() + args
        return func, args

    def call_func(self, func, args):
        func(*args)

    def print_banner(self):
        # The method is intentionally left empty
        # because not all subclasses require a banner.
        # Subclasses can override this method if needed.
        pass

    def process_one_line(self):  
        """  
        Process a single command line from input.  
        
        This method handles both interactive and non-interactive modes,  
        reads input, executes commands, and manages exception handling.  
        """  
        # Read input line from appropriate source  
        line = self._read_input_line()  
        if line is None:  
            return  
        
        # Process non-empty commands  
        line = line.strip()  
        if line:  
            self.last_cmd = line  
            self._execute_command(line)  
    
    def _read_input_line(self):  
        """  
        Read input line from either interactive prompt or file input.  
        
        Returns:  
            str: The input line, or None if interrupted  
        """  
        if self.interactive:  
            return self._read_interactive_input()  
        else:  
            return self._read_file_input()  
    
    def _read_interactive_input(self):  
        """  
        Read input from interactive prompt with Python 2/3 compatibility.  
        
        Returns:  
            str: The input line, or None if KeyboardInterrupt occurs  
        """  
        try:  
            # Handle Python 2/3 compatibility for input function  
            try:  
                prompt = raw_input  # Python 2  
            except NameError:  
                prompt = input      # Python 3  
            return prompt(self.get_prompt())  
        except KeyboardInterrupt:  
            self.fout.write('\n')  
            return None  
    
    def _read_file_input(self):  
        """  
        Read input from file stream.  
        
        Returns:  
            str: The input line  
            
        Raises:  
            EOFError: When end of file is reached  
        """  
        line = self.fin.readline()  
        if len(line) == 0:  
            raise EOFError()  
        return line  
    
    def _execute_command(self, line):  
        """  
        Execute a command line with proper exception handling.  
        
        Args:  
            line: The command line to execute  
        """  
        try:  
            # Find and execute the command  
            cmd = self.find_cmd(line + ' ')  
            func, args = self.bind_args(cmd, line)  
            self.call_func(func, args)  
            
        except self.HandledError:  
            # Already handled errors - just pass  
            pass  
    
        except self.InvalidCommandError:  
            # Invalid command - just pass  
            pass  
    
        except self.BindError as e:  
            # Argument binding errors  
            self.err(e)  
    
        except self.CommandError as e:  
            # General command errors  
            self.err(e)  
    
        except Exception as e:  
            # Handle any other exception in non-interactive mode  
            if not self.interactive:  
                self.stop_loop = True  
            raise

    def save_history(self):
        if self.interactive and self.rl and self.history_file:
            try:
                self.rl.write_history_file(self.history_file)
            except OSError:
                self.err('Cannot write to history file "%s"' %
                         self.history_file)
            except Exception as e:
                self.err('Unexpected error saving history file "%s": %s' %
                        (self.history_file, e))

    def disable_echoctl(self):
        try:
            # termios module might not be available. Ignore ImportError if so.
            import termios

            self.old_flags = termios.tcgetattr(sys.stdin)
            new_flags = self.old_flags
            new_flags[3] &= ~termios.ECHOCTL
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, new_flags)
        except ImportError:
            pass
        except Exception as e:
            print(f"Unexpected terminal error: {e}")

    def restore_echoctl(self):
        try:
            import termios

            cur_flags = termios.tcgetattr(sys.stdin)
            new_flags = cur_flags
            if self.old_flags[3] & termios.ECHOCTL:
                new_flags[3] |= termios.ECHOCTL
            else:
                new_flags[3] &= ~termios.ECHOCTL
            termios.tcsetattr(sys.stdin, termios.TCSADRAIN, new_flags)
        except ImportError:
            pass
        except Exception as e:
            print(f"Unexpected terminal error: {e}")

    def go_interactive(self):
        try:
            import readline
            self.rl = readline
        except ImportError:
            self.err('"readline" not available. No auto completion.\n')
            return

        if 'libedit' in self.rl.__doc__:
            self.rl.parse_and_bind('bind -e')
            self.rl.parse_and_bind("bind '\t' rl_complete")
        else:
            self.rl.parse_and_bind('tab: complete')

        self.rl.set_completer(self.complete)

        # Remove `~!@#$%^&*()-=+[{]}\|;:'",<>?/ from readline delimiters
        # leaving only space, tab, LF
        self.rl.set_completer_delims(' \x09\x0a')

        try:
            if self.history_file and os.path.exists(self.history_file):
                self.rl.read_history_file(self.history_file)
        except OSError:
            self.err('Cannot read from history file "%s"' %
                     self.history_file)
        except Exception as e:
            self.err('Unexpected error reading history file "%s": %s' %
                    (self.history_file, e))

        self.print_banner()
        self.fout.flush()

    def loop(self):
        self.disable_echoctl()

        try:
            self.stop_loop = False

            # the main command loop
            while not self.stop_loop:
                self.process_one_line()
        except EOFError:
            if self.interactive:
                self.fout.write('\n')
        finally:
            self.save_history()
            self.restore_echoctl()
