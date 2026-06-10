# Unicorn LCA Graph Engine

Open `index.html` in a browser and load:

- the LCA output or `.bdamage.txt` file
- `nodes.dmp`
- optionally `names.dmp`

The viewer builds the induced taxonomy tree for taxa that received LCA placements and all of their ancestors. Circle radius can represent either direct reads assigned to a node or cumulative reads assigned to that node plus all descendants.

The LCA parser accepts both per-query rows:

```text
query_name<TAB>taxid<TAB>name
```

and summary rows:

```text
#taxid<TAB>count<TAB>name
taxid<TAB>count<TAB>name
```
