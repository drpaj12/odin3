attribute \top 1
module \t
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire input 3 \s
  wire width 4 output 4 \y
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  cell $and $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n38 $n37 $n36 $n35 }
  end
  cell $or $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n42 $n41 $n40 $n39 }
  end
  attribute \full_case 1
  cell $mux $c9
    parameter \WIDTH 1
    connect \A $n42
    connect \B $n38
    connect \S \s
    connect \Y \y [3]
  end
  attribute \full_case 1
  cell $mux $c10
    parameter \WIDTH 1
    connect \A $n41
    connect \B $n37
    connect \S \s
    connect \Y \y [2]
  end
  attribute \full_case 1
  cell $mux $c11
    parameter \WIDTH 1
    connect \A $n40
    connect \B $n36
    connect \S \s
    connect \Y \y [1]
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 1
    connect \A $n39
    connect \B $n35
    connect \S \s
    connect \Y \y [0]
  end
end
