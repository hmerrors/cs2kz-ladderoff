from ambuild2 import run
import os


parser = run.BuildParser(sourcePath=os.path.dirname(__file__), api='2.2')
parser.options.add_argument('--mms-path', required=True, help='Metamod:Source source directory')
parser.options.add_argument('--sdk-path', required=True, help='Source 2 CS2 SDK source directory')
parser.Configure()
