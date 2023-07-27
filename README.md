# Asynchronous File Loader

Python asynchronous file loader module implemented in C/CPython.

## Installation

### Requirements

* Install liburing (follow the instructions at https://github.com/axboe/liburing).
* Ensure kernel is sufficently up-to-date for liburing (check version with `uname -r`).
  * To update, run `sudo apt-get update` and `sudo apt-get upgrade `.
  * Reboot to take effect (`sudo reboot`).

### Manual

```python setup.py install```

*Note: you may need to `chown` the created build folder for proper permissions.*

## Documentation

### `AsyncLoader.Loader(queue_depth: int, max_file_size: int, n_workers: int, min_dispatch_n: int)`

Loader, responsible for handling up to `queue_depth` concurrent requests
per worker, with up to `n_workers` workers. `min_dispatch_n` is currently
unused, but is intended to be a minimum number of IO requests the loader must
issue at once (to better utilize IO sorting).

#### `Loader.become_loader()`

Causes this process to be used as the loader, spawning the loader and responder
threads. Does not return.

#### `Loader.spawn_loader()`

Causes this process to fork, with the child becoming the loader process.
Equivilent to spawning a new process and calling `become_loader()`.

#### `Loader.get_worker_context(id: int) -> AsyncLoader.Worker`

Returns the `AsyncLoader.Worker` context for the given worked id.

### `AsyncLoader.Worker`

Worker context. Provides an interface to the loader for the given worker.

#### `Worker.request(filepath: str) -> bool`

Request a filepath to be loaded.

#### `Worker.try_get() -> AsyncLoader.Entry`

Attempt to fetch an entry from the completion queue. If an entry is available,
returns an `AsyncLoader.Entry`. If no entry is available, `None` is returned.

#### `Worker.wait_get() -> AsyncLoader.Entry`

Spin on `try_get()` until an entry is returned.

### `AsyncLoader.Entry`

Entry class. Returned by `try_get` and `wait_get` methods, containing file data
for the requested IO. Once the data has been used, the entry should be released
by calling `Entry.Release()`, otherwise the loader will run out of entries.

#### `Entry.get_filepath() -> str`

Return the filepath that was loaded for this entry.

#### `Entry.get_data() -> bytes`

Returns the contained filedata as `bytes`.

#### `Entry.release()`

Releases the entry. Should always be called once `get_data()` has been called
for the final time. Failure to release entries will eventually prevent new
requests from being submitted, as no entries will be free to house them.


## Diagram

<p align="center">
    <img src="./diagram.svg">
</p>