attribute \top 1
module \t
  wire input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 input 5 \d
  wire width 8 output 6 \y
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  cell $mux $c9
    parameter \WIDTH 8
    connect \A { \d $n41 $n40 $n39 $n38 }
    connect \B { \d $n41 $n40 $n39 $n38 }
    connect \S \s
    connect \Y \y
  end
  cell $mux $c10
    parameter \WIDTH 4
    connect \A \c
    connect \B \b
    connect \S \s
    connect \Y { $n37 $n36 $n35 $n34 }
  end
  cell $and $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n37 $n36 $n35 $n34 }
    connect \Y { $n41 $n40 $n39 $n38 }
  end
end
