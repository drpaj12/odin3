attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 output 5 \y
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  cell $mux $c8
    parameter \WIDTH 4
    connect \A { $n33 $n32 $n31 $n30 }
    connect \B { $n33 $n32 $n31 $n30 }
    connect \S \s
    connect \Y \y
  end
  cell $mux $c9
    parameter \WIDTH 4
    connect \A \c
    connect \B \b
    connect \S \s
    connect \Y { $n29 $n28 $n27 $n26 }
  end
  cell $add $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n29 $n28 $n27 $n26 }
    connect \Y { $n33 $n32 $n31 $n30 }
  end
end
