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

from __future__ import print_function
import re
import tokenize
import io

'''
<BESS script language>
- Providing a Click-like module connection semantics
- All these syntactic sugars must be able to coexist with original Python
  syntax. e.g.,
    print('hello %s' % (%ENV!'Anynomous'))

--------------------------------------------------------------------------------
Syntax               Semantic                      Plain Python code
--------------------------------------------------------------------------------
$ENV                 The value of environment      __bess_env__('ENV')
                     variable ENV (string)
                     No empty string exists

$ENV!str             If ENV not exists, use x      __bess_env__('ENV', str)
                     (x can be a string literal,
                     variable of a string literal,
                     or parenthesis string literal
                     expression)

a::SomeModule()      Create a SomeModule and       __bess_module__('a',
                     assign into variable a          'SomeModule')

a::SomeModule(...)   Additional parameters for     __bess_module__('a',
                     creating module                 'SomeModule', ...)

a,b::SomeModule()    Create two SomeModule and     __bess_module__(('a','b'),
                     assign into a and b             'SomeModule')

a->b                 Connect a and b               a + b

a:x->b               Connect output gate x of a    a*i + b
                     and b                         a.connect(next_mod=b,
                     (x should be an integer)        ogate=x)

a->y:b               Connect a to input gate y     a + y*b
                     of b                          a.connect(next_mod=b,
                     (y should be an integer)        igate=y)

a:3->4:b             Connect output gate 3 of a    a*3 + 4*b
                     and input gate of 4           a.connect(next_mod=b,
                                                     ogate=3, igate=4)

--------------------------------------------------------------------------------

--------------------------------------------------------------------------------
Usage examples
--------------------------------------------------------------------------------

1. Chaining multiple module connection
Ringo:
    a -> b:1 -> c
Python:
    a + b*1 + c

2. Create and connect at once
Ringo:
    a::Foo() -> b::Bar()
Python:
    __bess_module__('a','Foo') + __bess_module__('b', 'Bar')

3. Create anonymous modules and connect them
Ringo:
    Foo() -> Bar()
Python:
    Foo() + Bar()

4. Composition example
Ringo:
    a::Foo():3 -> b
Python:
    __bess_module__('a', 'Foo')*3 + b

5.
a::Foo():1 -> 2:b::Foo()
- Connecting output gate 1 of a to input gate 2 of b
'''

# common python tokens
NAME = r'[a-zA-Z_]\w*'
COMMENT = r'#[^\r\n]*'
STRING_SHORT = r'\'.*?\'|\".*?\"'
STRING_LONG = r'\'\'\'.*?\'\'\'|""".*?"""'
STRING_ALL = STRING_LONG + '|' + STRING_SHORT

# constants for duplicate literals
BESS_ENV_PREFIX = "__bess_env__('" 

def replace_envvar(s):
    environment = r'\$(' + NAME + ')'\
        r'(!(' + STRING_SHORT + '|' + NAME + '))?' \
        r'(!\()?'

    # first group: # leading COMMENT -> skip
    # second group: single / double /triple quoted strings -> skip
    # third group: dollar sign with NAME -> $var!assignment
    # third group consists of
    #   fourth group: environment variable
    #   fifth group:
    #   e.g., $ENV!'100'
    #       third group  "$ENV!'100'"
    #       fourth group 'ENV'
    #       fifth group '!' or not

    pattern = '(' + COMMENT + ')|(' + STRING_ALL + ')|(' + environment + ')'
    regex = re.compile(pattern, re.MULTILINE | re.DOTALL)

    def _replacer(match):
        if match.group(3) is not None:
            # from our definition of pattern
            # if match.group(3) is not None, match.group(4) is not None
            # if match.group(5) is not None, then there is a parameter
            if match.group(5) is None and match.group(7) is None:
                return BESS_ENV_PREFIX + match.group(4) + "')"
            elif match.group(5) is not None:
                return BESS_ENV_PREFIX + match.group(4) + "', " + \
                    match.group(6) + ")"
            else:
                return BESS_ENV_PREFIX + match.group(4) + "', "

        else:
            return match.group()

    s = regex.sub(_replacer, s)
    return s


def is_gate_expr(exp, is_ogate):
    # check if the leading/trailing whitespace characters contains '\n'
    if is_ogate:
        prefix, postfix = '1*', '+1'
    else:
        prefix, postfix = '1+', '*1'

    exp_stripped = exp.strip()
    while len(exp_stripped) > 0 and exp_stripped[-1] == '\\':
        exp_stripped = exp_stripped[:-1].strip()

    try:
        compile('(%s)' % exp_stripped, '', mode='eval')
        compile('%s%s%s' % (prefix, exp, postfix), '', mode='eval')
    except SyntaxError:
        return False
    else:
        return True


def replace_rarrows(s):  
    """  
    Replace arrow syntax (->) with Python operations in BESS scripts.  
      
    This function transforms BESS's arrow-based module connection syntax  
    into Python code using the + and * operators for module connections  
    and gate specifications.  
      
    Args:  
        s: Input string containing BESS script with arrow syntax  
          
    Returns:  
        str: Transformed Python code with module connections  
    """  
    # Phase 1: Find all arrow positions and split the string into segments  
    arrow_positions = _find_arrow_positions(s)  
    segments = _split_by_arrows(s, arrow_positions)  
      
    # Phase 2: Transform gate specifications in each segment  
    _transform_gate_specifications(segments)  
      
    return '+'.join(segments)  
  
def _find_arrow_positions(s):  
    """  
    Find all positions of arrow operators (->) in the input string.  
      
    Uses lexical analysis to properly identify arrows while avoiding  
    false positives in string literals or comments.  
      
    Args:  
        s: Input string to analyze  
          
    Returns:  
        list: List of (row, col) tuples indicating arrow positions  
    """  
    arrow_positions = []  
    last_token = None  
      
    try:  
        for t in tokenize.generate_tokens(io.StringIO(s).readline):  
            token = t[1]  
            row, col = t[2]  
              
            # Handle both Python 2.x and Python 3 tokenization  
            if last_token == '-' and token == '>':  # Python 2.x  
                arrow_positions.append((row - 1, col - 1))  
            elif token == '->':  # Python 3  
                arrow_positions.append((row - 1, col))  
              
            last_token = token  
              
    except (tokenize.TokenError, IndentationError):  
        # Source code has syntax errors, but arrows have been found  
        # correctly up until this point  
        pass  
      
    return arrow_positions  
  
def _split_by_arrows(s, arrow_positions):  
    """  
    Split the input string into segments based on arrow positions.  
      
    Args:  
        s: Input string to split  
        arrow_positions: List of arrow positions from _find_arrow_positions  
          
    Returns:  
        list: List of string segments between arrows  
    """  
    segments = []  
    curr_seg = []  
    arrow_idx = 0  
      
    lines = io.StringIO(s).readlines()  
    line_idx = 0  
    col_offset = 0  
      
    while line_idx < len(lines):  
        line = lines[line_idx]  
          
        if arrow_idx < len(arrow_positions):  
            row, col = arrow_positions[arrow_idx]  
        else:  
            row, col = None, None  
          
        if row is None or line_idx < row:  
            # No arrow in this line, add entire line  
            curr_seg.append(line[col_offset:])  
            line_idx += 1  
            col_offset = 0  
        elif line_idx == row:  
            # Arrow found in current line, split at arrow position  
            curr_seg.append(line[col_offset:col])  
            segments.append(''.join(curr_seg))  
            curr_seg = []  
            col_offset = col + 2  # Skip past '->'  
            arrow_idx += 1  
        else:  
            assert False  
      
    segments.append(''.join(curr_seg))  
    return segments  
  
def _transform_gate_specifications(segments):  
    """  
    Transform output gate (:xx ->) and input gate (-> :yy) specifications.  
      
    Converts gate syntax into Python multiplication operations for  
    proper gate specification in module connections.  
      
    Args:  
        segments: List of segments to transform (modified in-place)  
    """  
    for i in range(len(segments) - 1):  
        # Process output gate specification in current segment  
        _process_output_gate(segments, i)  
          
        # Process input gate specification in next segment  
        _process_input_gate(segments, i)  
  
def _process_output_gate(segments, segment_idx):  
    """  
    Process output gate specification (:xx ->) in a segment.  
      
    Args:  
        segments: List of segments (modified in-place)  
        segment_idx: Index of the segment to process  
    """  
    seg = segments[segment_idx]  
    colon_pos = seg.rfind(':')  
      
    while colon_pos != -1:  
        ogate = seg[colon_pos + 1:]  
          
        if ogate.strip() == '':  
            break  
          
        if is_gate_expr(ogate, True):  
            # Transform :ogate to *parenthesize(ogate)  
            segments[segment_idx] = seg[:colon_pos] + '*' + _parenthesize(ogate)  
            break  
          
        # Look for previous colon  
        colon_pos = seg.rfind(':', 0, colon_pos)  
  
def _process_input_gate(segments, segment_idx):  
    """  
    Process input gate specification (-> :yy) in a segment.  
      
    Args:  
        segments: List of segments (modified in-place)  
        segment_idx: Index of the segment to process  
    """  
    seg = segments[segment_idx + 1]  
    colon_pos = seg.find(':')  
      
    while colon_pos != -1:  
        igate = seg[:colon_pos]  
          
        if igate.strip() == '':  
            break  
          
        if is_gate_expr(igate, False):  
            # Transform igate: to parenthesize(igate)*  
            segments[segment_idx + 1] = _parenthesize(igate) + '*' + seg[colon_pos + 1:]  
            break  
          
        # Look for next colon  
        colon_pos = seg.find(':', colon_pos + 1)  
  
def _parenthesize(exp):  
    """  
    Add parentheses around an expression if needed for complex expressions.  
      
    Args:  
        exp: Expression to potentially parenthesize  
          
    Returns:  
        str: Parenthesized expression if needed, original expression otherwise  
    """  
    for t in tokenize.generate_tokens(io.StringIO(exp).readline):  
        if t[0] == tokenize.OP:  
            l = len(exp) - len(exp.lstrip())  
            r = len(exp) - len(exp.rstrip())  
            return '%s(%s)%s' % (exp[:l], exp.strip(), exp[len(exp) - r:])  
    return exp

def create_module_string(s):

    # single module -> return a module name string
    if s.find(',') < 0:
        return "'" + s + "'"

    # multiple module -> return a tuple of module name string
    mstr = '('
    for module in s.split(','):
        mstr += "'" + module.strip() + "', "
    mstr += ')'
    return mstr


def replace_module_assignment(s):
    target = '(' + NAME + r'(,[ \t]*' + NAME + ')*' + ')::(' + NAME + r')[ \t]*\('

    # first group: # leading COMMENT -> skip
    # second group: single / double / triple quoted strings -> skip
    # third group: replace target  'NAME::NAME'
    pattern = '(' + COMMENT + ')|(' + STRING_ALL + ')|(' + target + ')'
    regex = re.compile(pattern, re.MULTILINE | re.DOTALL)

    def _replacer(match):
        if match.group(3) is not None:
            # if match.group(3) is not None,
            # match.group(4), match.group(6) is not None
            # match.group(4) -> module NAMEs
            # match.group(6) -> module class NAME
            modules = create_module_string(match.group(4))
            f_str = "__bess_module__(" + modules + ", '" + \
                    match.group(6) + "', "
            return f_str

        else:
            return match.group()

    return regex.sub(_replacer, s)


def xform_str(s):
    s = replace_envvar(s)
    s = replace_module_assignment(s)
    s = replace_rarrows(s)
    return s


def xform_file(filename):
    with io.open(filename) as f:
        return xform_str(f.read())
