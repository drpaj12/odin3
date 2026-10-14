attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire output 5 \y
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  cell $mux $c8
    parameter \WIDTH 1
    connect \A $n21
    connect \B $n21
    connect \S \s
    connect \Y \y
  end
  cell $mux $c9
    parameter \WIDTH 4
    connect \A \c
    connect \B \b
    connect \S \s
    connect \Y { $n20 $n19 $n18 $n17 }
  end
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B { $n20 $n19 $n18 $n17 }
    connect \Y $n21
  end
end
