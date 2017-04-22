lwaio
=====

Light-weight asynchronous I/O


What is lwaio?
--------------

The lwaio library is a light-weight library for asynchronous input/output
(I/O). lwaio replicates the basic functionality of the POSIX aio library with
the following additional features: it only spawns one thread to perform the I/O
operations, and this thread may be pinned to a specific core.


Requirements
------------

- C compiler


Installation/Usage
------------------

1. Clone this repository
1. Compile and link to your code

