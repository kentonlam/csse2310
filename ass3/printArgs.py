#!/usr/bin/env python3

import sys

if __name__ == '__main__':
    print('@', flush=True, end='')
    print(sys.argv, file=sys.stderr)
