"""
   MIT License

   Copyright (c) 2023 Gus Waldspurger

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
"""

import os
import sys
import time
import math
import multiprocessing as mp
import AsyncLoader as al
from typing import List
from glob import glob

# Get all filepaths descending from the provided ROOT directory.
def get_all_filepaths(root: str, extension: str = "*"):
    # Taken from https://stackoverflow.com/a/18394205
    return [y for x in os.walk(root) for y in glob(os.path.join(x[0], "*.{}".format(extension)))]

# Load all files in FILEPATHS using regular synchronous IO.
def load_normal(filepaths: List[str]):
    for filepath in filepaths:
        with open(filepath, 'r') as file:
            file.read(-1) # Read the entire file

# Load all files in FILEPATHS using an AsyncLoader worker context.
def load_async_worker_loop(filepaths: List[str], worker: al.Worker):
    # Submit all requests
    for filepath in filepaths:
        worker.request(filepath)

    # Read all images
    for _ in range(len(filepaths)):
        entry = worker.wait_get()
        entry.release()

# Load all files in FILEPATHS using AsyncLoader with N_WORKERS worker threads.
def load_async(filepaths: List[str], max_file_size: int, n_workers: int):
    n_files = len(filepaths)
    files_per_loader = int(math.ceil(n_files / n_workers))
    loader = al.Loader(queue_depth=files_per_loader,
                       max_file_size=max_file_size,
                       n_workers=n_workers,
                       min_dispatch_n=-1)
    
    # Spawn the loader
    loader_process = mp.Process(target=loader.become_loader)
    
    # Spawn N_WORKERS workers
    processes = []
    for i in range(n_workers):
        process = mp.Process(target=load_async_worker_loop, args=(
            filepaths[i * files_per_loader : (i + 1) * files_per_loader],
            loader.get_worker_context(id=i)
        ))
        processes.append(process)

    # Start all the processes
    loader_process.start()
    for process in processes:
        process.start()

    # Wait on the workers
    for process in processes:
        process.join()

    # Kill the loader once the workers are done
    loader_process.kill()

def main():

    if len(sys.argv) < 2:
        print("Please provide the filepath of a directory to load from.")
        return
    if len(sys.argv) < 3:
        print("Please provide the desired file extension to be loaded.")
        return
    
    filepath = sys.argv[1]
    extension = sys.argv[2]
    filepaths = get_all_filepaths(filepath, extension)
    max_size = ((max([os.path.getsize(path) for path in filepaths]) // (1024 * 4)) + 1) * 1024 * 4
    print("Max size: {}\nFilepaths: {}".format(max_size, filepaths))

    # Get normal loading time
    begin_normal = time.time()
    load_normal(filepaths)
    time_normal = time.time() - begin_normal
    print("Normal: {:.04}s.".format(time_normal))

    # Get async loading time(s)
    worker_configs = [1]
    for n_workers in worker_configs:
        begin_async = time.time()
        load_async(filepaths, max_size, n_workers)
        time_async = time.time() - begin_async
        print("AsyncLoader ({} workers): {:.04}".format(n_workers, time_async))

if __name__ == "__main__":
    main()