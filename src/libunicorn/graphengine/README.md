# Unicorn LCA Graph Engine

Open `index.html` in a browser and load:

- one or more LCA output or `.bdamage.txt` files
- optionally a text file listing input files, one path per line
- `nodes.dmp`
- optionally `names.dmp`

The viewer builds the induced taxonomy tree for taxa that received LCA placements and all of their ancestors. Node size can represent either direct reads assigned to a node or cumulative reads assigned to that node plus all descendants. When multiple input files are loaded, nodes are rendered as pie charts showing the proportion contributed by each file.

Path-list notes:

- one input path per line
- blank lines and `#` comments are ignored
- relative paths resolve from the page location

The LCA parser accepts both per-query rows:

```text
query_name<TAB>taxid<TAB>name
```

and summary rows:

```text
#taxid<TAB>count<TAB>name
taxid<TAB>count<TAB>name
```
