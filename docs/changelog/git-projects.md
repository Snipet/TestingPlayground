### Projects in git: Connect, Push, Pull, Check and Add from Git

**Added**
- **Git...** in the Projects card (and the project pill's right-click menu) keeps a project in a git repository of
  yours, through the git on your computer and its own sign-in (an SSH agent, a credential helper): the plugin asks for
  and keeps no password. **Connect** commits every version to an empty repository and pushes them; **Push** commits and
  pushes the versions saved since, never forcing; **Pull** brings what another machine pushed as a new version, and
  opens it when the project is open and unchanged; **Check** counts what there is to push and to pull; **Disconnect**
  removes the link and keeps the project and its folder.
- **Add from Git...** clones a repository and adds its newest commit as a new project, linked.
- A linked project has a branch mark after its name in the Projects card, the explorer and the top bar's pill, with
  arrows and counts for what there is to push and to pull.
- The Settings card's Projects page says which git runs (**Look for git**, **Choose git...**) and where the working
  folders are.
