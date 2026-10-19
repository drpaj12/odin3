attribute \top 1
module \t
  wire input 1 \s
  wire width 3 input 2 signed \a
  wire width 2 input 3 signed \b
  wire width 4 input 4 signed \c
  wire width 5 output 5 signed \y
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  cell $mux $c8
    parameter \WIDTH 5
    connect \A { $n34 $n33 $n32 $n31 $n30 }
    connect \B { $n34 $n33 $n32 $n31 $n30 }
    connect \S \s
    connect \Y \y
  end
  cell $mux $c9
    parameter \WIDTH 4
    connect \A \c
    connect \B { \b [1] \b [1] \b }
    connect \S \s
    connect \Y { $n29 $n28 $n27 $n26 }
  end
  cell $add $c10
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 3
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B { $n29 $n28 $n27 $n26 }
    connect \Y { $n34 $n33 $n32 $n31 $n30 }
  end
end
