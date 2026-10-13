attribute \top 1
module \t
  wire input 1 \s
  wire input 2 \u
  wire width 4 input 3 \a
  wire width 4 input 4 \b
  wire width 4 output 5 \y
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  cell $xor $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n30 $n29 $n28 $n27 }
  end
  attribute \full_case 1
  cell $mux $c9
    parameter \WIDTH 4
    connect \A { $n30 $n29 $n28 $n27 }
    connect \B { $n26 $n25 $n24 $n23 }
    connect \S \s
    connect \Y \y
  end
  attribute \full_case 1
  cell $mux $c10
    parameter \WIDTH 4
    connect \A \a
    connect \B \b
    connect \S \u
    connect \Y { $n39 $n38 $n37 $n36 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { $n39 $n38 $n37 $n36 }
    connect \S \s
    connect \Y { $n26 $n25 $n24 $n23 }
  end
end
