attribute \top 1
module \t
  wire input 1 \en
  wire width 4 input 2 \d
  wire width 8 output 3 \y
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  cell $not $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \d
    connect \Y { $n29 $n28 $n27 $n26 }
  end
  attribute \full_case 1
  cell $mux $c8
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { $n29 $n28 $n27 $n26 }
    connect \S \en
    connect \Y \y [3:0]
  end
  connect \y [7:4] \d
end
