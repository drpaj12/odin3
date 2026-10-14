module \top
  wire width 4 input 1 \x
  wire width 4 input 2 \z
  wire width 4 output 3 \o
  wire width 4 output 4 \p
  wire width 4 \m1.a
  wire width 4 \m1.b
  wire width 4 \m1.y
  wire width 4 \m2.a
  wire width 4 \m2.b
  wire width 4 \m2.y
  wire width 4 \m1.u.a
  wire width 4 \m1.u.b
  wire width 4 \m1.u.y
  wire width 4 \m1.u.t
  wire width 4 \m2.u.a
  wire width 4 \m2.u.b
  wire width 4 \m2.u.y
  wire width 4 \m2.u.t
  cell $xor $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \m1.u.t
    connect \B \x
    connect \Y \o
  end
  cell $and $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \x
    connect \B \z
    connect \Y \m1.u.t
  end
  cell $xor $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \m2.u.t
    connect \B \z
    connect \Y \p
  end
  cell $and $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \z
    connect \B \x
    connect \Y \m2.u.t
  end
  connect \m1.a \x
  connect \m1.b \z
  connect \m1.y \o
  connect \m2.a \z
  connect \m2.b \x
  connect \m2.y \p
  connect \m1.u.a \x
  connect \m1.u.b \z
  connect \m1.u.y \o
  connect \m2.u.a \z
  connect \m2.u.b \x
  connect \m2.u.y \p
end
