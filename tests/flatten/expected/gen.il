module \top
  wire width 3 input 1 \x
  wire width 3 input 2 \z
  wire width 3 output 3 \o
  wire output 4 \c
  wire width 3 \m.a
  wire width 3 \m.b
  wire width 3 \m.y
  wire \sel.v.a
  wire \sel.v.b
  wire \sel.v.y
  wire \m.g[0].u.a
  wire \m.g[0].u.b
  wire \m.g[0].u.y
  wire \m.g[1].u.a
  wire \m.g[1].u.b
  wire \m.g[1].u.y
  wire \m.g[2].u.a
  wire \m.g[2].u.b
  wire \m.g[2].u.y
  cell $xor $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x [0]
    connect \B \z [2]
    connect \Y \c
  end
  cell $xor $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x [0]
    connect \B \z [0]
    connect \Y \o [0]
  end
  cell $xor $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x [1]
    connect \B \z [1]
    connect \Y \o [1]
  end
  cell $xor $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x [2]
    connect \B \z [2]
    connect \Y \o [2]
  end
  connect \m.a \x
  connect \m.b \z
  connect \m.y \o
  connect \sel.v.a \x [0]
  connect \sel.v.b \z [2]
  connect \sel.v.y \c
  connect \m.g[0].u.a \x [0]
  connect \m.g[0].u.b \z [0]
  connect \m.g[0].u.y \o [0]
  connect \m.g[1].u.a \x [1]
  connect \m.g[1].u.b \z [1]
  connect \m.g[1].u.y \o [1]
  connect \m.g[2].u.a \x [2]
  connect \m.g[2].u.b \z [2]
  connect \m.g[2].u.y \o [2]
end
