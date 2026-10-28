# ESP-IDF's CMake asks git for a revision of the project and of the framework
# (installed under ~/.platformio). If a parent folder such as $HOME is itself
# a git repo without commits, that lookup fails the build. Stop git discovery
# at $HOME.
import os

Import("env")  # noqa: F821

os.environ["GIT_CEILING_DIRECTORIES"] = os.path.expanduser("~")
