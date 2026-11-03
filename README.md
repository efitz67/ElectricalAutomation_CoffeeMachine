# ElectricalAutomation_CoffeeMachine
This is a PLC project that aims to automate a coffee machine for the module Electrical Automation in the University of Limerick.





Git instructions for neatly managing branches:
Check for changes within your own branch:

- git fetch

If it tells you there are changes, import the changes:

- git pull

Navigating to a branch (type without << >>):

- git checkout <<branch_name>>

Making a new branch (type without << >>):

- git checkout -b <<new_branch_name>>

For safety, always check the status of your current branch first:

- git status

Only the files you have edited should be red. If other files are also red, make sure there is nothing wrong with those files.

Adding code to a commit (type without << >>):

- git add <<filename.fileextension>>

If you wish to add every change you made to the commit, simply type:

- git add *

Next, commit your added changes accompanied by a short description of the changes (include the quotation marks):

- git commit -m "your short description of the changes"

Once git accepts your commit (no issues), you can now push it upstream (so other people can pull your changes):

- git push

The very first time you wish to push to a branch, you must couple it to the remote branch for git to establish the connection.

This goes as follows (type without << >>):

- git push --set-upstream origin <<branch_name>>

You will only need to do this once per branch.

Always ensure you are in the correct branch, and the status of your branch is caught up before committing or pushing.

For safety reasons, it is not possible to push directly to the main branch.

When committing changes, make sure you are in a branch that is relevant to those changes.

If not, make one.
