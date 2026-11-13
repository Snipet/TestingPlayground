### Keep a project in a git repository

**Added**
- **Git...** in the Projects card (and the top bar's project pill's right-click menu) keeps a project in a git
  repository of yours: give it an empty repository's address and **Connect**, and each saved version becomes a commit,
  pushed. From then on **Push** sends the versions you have saved since, **Pull** brings new commits in as new versions
  (the newest opened when the editor has no unsaved change), and **Check** says how far ahead and behind you are. A
  linked project shows a small branch mark after its name, in the Projects card, the explorer and the top bar's pill,
  with the versions to push and the commits to pull when they are known.
- **Add from Git** makes a new project here from a repository another machine pushed to: the Projects card's **Git...**
  with no project chosen, or the top bar's project pill's right-click menu while no project is open.
- **Disconnect** removes the link; the working folder stays where it is (nothing is deleted), and **Show folder** opens
  it.
- The Settings card's Projects page says which git the projects' git uses: **Find git** looks for it, **Choose git...**
  picks one, and it can go back to the usual places. Git signs in its own way (an SSH agent, a credential helper); the
  plugin keeps no password, and runs git only when you press Connect, Push, Pull, Check or Add from Git.
