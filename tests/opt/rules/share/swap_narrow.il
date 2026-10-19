attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 signed \a
  wire width 3 input 3 signed \b
  wire width 4 input 4 signed \c
  wire width 5 output 5 signed \y
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  cell $mux $c8
    parameter \WIDTH 5
    connect \A { $n36 $n35 $n34 $n33 $n32 }
    connect \B { $n36 $n35 $n34 $n33 $n32 }
    connect \S \s
    connect \Y \y
  end
  cell $mux $c9
    parameter \WIDTH 4
    connect \A \c
    connect \B { \b [2] \b }
    connect \S \s
    connect \Y { $n31 $n30 $n29 $n28 }
  end
  cell $add $c10
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B { $n31 $n30 $n29 $n28 }
    connect \Y { $n36 $n35 $n34 $n33 $n32 }
  end
end
