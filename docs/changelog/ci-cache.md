### Faster builds on every pull request

**Changed**
- The builds on GitHub's machines keep bgfx's shader compiler between runs, on macOS, Linux and Windows, and on macOS
  and Linux the compiled objects too (ccache): a run whose code changed little rebuilds little. `main` keeps the
  caches fresh; a pull request saves its own only when it found none to start from. Each run's log says how many
  objects came from the cache.
- A pull request that changes only the docs (`docs/` and the `.md` files beside this one) builds and tests nothing:
  its checks pass at once.
