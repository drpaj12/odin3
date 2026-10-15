module \top
  wire input 1 \x
  wire output 2 \o
  wire output 3 \p
  wire output 4 \q
  wire \t.a
  wire \t.y
  wire \d.a
  wire \d.y
  wire \d.k
  wire \e.a
  wire \e.y
  wire \e.k
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x
    connect \Y \p
  end
  cell $not $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \x
    connect \Y \e.y
  end
  connect \o \x
  connect \q 1'1
  connect \t.a \x
  connect \t.y \x
  connect \d.a \x
  connect \d.y \p
  connect \d.k 1'1
  connect \e.a \x
  connect \e.k 1'1
end
