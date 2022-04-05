# A Minimal HTTP Server For Learning Purpose

## Main Featuress

- Implemented methods: `GET`, `HEAD`, `POST`
- Supported static file types: `html`, `gif`, `png`, `jpg`, `mp4`, `plain text`
- Concurrency supported
- Arguments parsing implemented for dynamic contents using `GET` and `POST` methods

## Compile

```bash
gcc mini_server.c -o mini_server
```

## References

- Computer Systems: A Programmer's Perspective, 3rd Edition, [tiny.c](http://csapp.cs.cmu.edu/3e/ics3/code/netp/tiny/tiny.c)
- Hacking: The Art of Exploitation, 2nd Edition, `tinyweb.c`
