### Faster Windows builds on every pull request

**Changed**
- The builds on GitHub's Windows machines keep their compiled objects between runs too (sccache, as macOS and Linux
  keep theirs with ccache): a run whose code changed little rebuilds little. The log says how many objects came from
  the cache and how large it is.
- A build run by hand can build only some of the platforms (`platforms`: `windows-x64`, say), so a branch's Windows
  build can be tried without a macOS machine.
