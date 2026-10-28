# The home directory is a git repo without commits; ESP-IDF's CMake asks git
# for a revision, finds ~/.git and fails. Stop git discovery at $HOME.
import os

Import("env")  # noqa: F821

os.environ["GIT_CEILING_DIRECTORIES"] = os.path.expanduser("~")
