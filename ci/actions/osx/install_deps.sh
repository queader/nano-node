#!/bin/bash

brew update
brew install coreutils
brew install qt@5
brew link --force qt@5
# workaround for homebrew bug
export PATH=/usr/local/opt/qt5/bin:$PATH
brew install --cask xquartz