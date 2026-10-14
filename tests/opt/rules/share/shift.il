attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 2 input 3 \b
  wire width 2 input 4 \c
  wire width 4 output 5 \y
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  cell $mux $c8
    parameter \WIDTH 4
    connect \A { $n27 $n26 $n25 $n24 }
    connect \B { $n27 $n26 $n25 $n24 }
    connect \S \s
    connect \Y \y
  end
  cell $mux $c9
    parameter \WIDTH 2
    connect \A \c
    connect \B \b
    connect \S \s
    connect \Y { $n23 $n22 }
  end
  cell $shl $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n23 $n22 }
    connect \Y { $n27 $n26 $n25 $n24 }
  end
end
