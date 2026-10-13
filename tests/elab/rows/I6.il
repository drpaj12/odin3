# generated: elab-check --write
attribute \top 1
module \i6_instance_array
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire width 4 output 3 \z
  cell \i6_inv \u[3]
    connect \a \a [3]
    connect \y \y [3]
  end
  cell \i6_inv \u[2]
    connect \a \a [2]
    connect \y \y [2]
  end
  cell \i6_inv \u[1]
    connect \a \a [1]
    connect \y \y [1]
  end
  cell \i6_inv \u[0]
    connect \a \a [0]
    connect \y \y [0]
  end
  cell \i6_inv \v[0]
    connect \a \a [0]
    connect \y \z [0]
  end
  cell \i6_inv \v[1]
    connect \a \a [1]
    connect \y \z [1]
  end
  cell \i6_inv \v[2]
    connect \a \a [2]
    connect \y \z [2]
  end
  cell \i6_inv \v[3]
    connect \a \a [3]
    connect \y \z [3]
  end
end
module \i6_inv
  wire input 1 \a
  wire output 2 \y
  cell $not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \Y \y
  end
end
