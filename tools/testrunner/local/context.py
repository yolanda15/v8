# Copyright 2022 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from contextlib import contextmanager
import os
import signal

import subprocess
import sys

from ..local.android import Driver
from .command import AndroidCommand, IOSCommand, PosixCommand, WindowsCommand, taskkill_windows
from .pool import DefaultExecutionPool
from ..testproc.util import list_processes_linux


class DefaultOSContext:

  def __init__(self, command, pool=None):
    self.command = command
    self.pool = pool or DefaultExecutionPool(self)

  @contextmanager
  def handle_context(self, options):
    yield

  def list_processes(self):
    return []

  def terminate_process(self, process):
    pass

  def platform_shell(self, shell, args, outdir):
    return os.path.abspath(os.path.join(outdir, shell))


class LinuxContext(DefaultOSContext):

  def __init__(self):
    super().__init__(PosixCommand)

  def list_processes(self):
    return list_processes_linux()

  def terminate_process(self, process):
    os.kill(process.pid, signal.SIGTERM)


class WindowsContext(DefaultOSContext):

  def __init__(self):
    super().__init__(WindowsCommand)

  def terminate_process(self, process):
    taskkill_windows(process, verbose=True, force=False)

  def platform_shell(self, shell, args, outdir):
    return os.path.abspath(os.path.join(outdir, shell)) + '.exe'


class AndroidOSContext(DefaultOSContext):

  def __init__(self):
    super().__init__(AndroidCommand)

  @contextmanager
  def handle_context(self, options):
    try:
      AndroidCommand.driver = Driver.instance(options.device)
      yield
    finally:
      AndroidCommand.driver.tear_down()


class IOSContext(DefaultOSContext):

  def __init__(self):
    super().__init__(IOSCommand)

  def terminate_process(self, process):
    os.kill(process.pid, signal.SIGTERM)

  def platform_shell(self, shell, appargs, outdir):
    # Rather than having to use a physical device (iPhone, iPad, etc), we use
    # the iOS Simulator for the test runners in order to ease the job of
    # builders and testers.
    # At the moment Chromium's iossim tool is being used, which is a wrapper
    # around 'simctl' macOS command utility.
    iossim = outdir + '/' + "iossim -d 'iPhone X' "

    if isinstance(appargs, list):
      appargs = ' '.join(map(str, appargs))
    if appargs != "":
      iossim = iossim + "-c "
      appargs = '\"' + appargs + '\"'

    app = outdir + '/' + shell + ".app"
    return iossim + appargs + " " + app

# TODO(liviurau): Add documentation with diagrams to describe how context and
# its components gets initialized and eventually teared down and how does it
# interact with both tests and underlying platform specific concerns.
def find_os_context_factory(target_os):
  registry = dict(
      android=AndroidOSContext, ios=IOSContext, windows=WindowsContext)
  default = LinuxContext
  return registry.get(target_os, default)


@contextmanager
def os_context(target_os, options):
  factory = find_os_context_factory(target_os)
  context_instance = factory()
  with context_instance.handle_context(options):
    yield context_instance
