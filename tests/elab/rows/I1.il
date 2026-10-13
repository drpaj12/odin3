# generated: elab-check --write
attribute \top 1
module \i1_instances
  wire input 1 \p
  wire input 2 \q
  wire output 3 \r1
  wire output 4 \r2
  wire output 5 \r3
  wire output 6 \r4
  wire $n7
  wire $n9
  cell \i1_leaf \u_ordered
    connect \a \p
    connect \b \q
    connect \y \r1
    connect \z \r2
  end
  cell \i1_leaf \u_named
    connect \a \p
    connect \b \q
    connect \y \r3
    connect \z $n7
  end
  cell \i1_leaf \u_empty
    connect \a \p
    connect \b 1'x
    connect \y \r4
    connect \z $n9
  end
end
module \i1_leaf
  wire input 1 \a
  wire input 2 \b
  wire output 3 \y
  wire output 4 \z
  cell $and $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y
  end
  cell $or $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \z
  end
end
