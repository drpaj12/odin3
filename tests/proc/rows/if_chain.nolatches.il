attribute \top 1
module \t
  wire width 3 input 1 \s
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 input 4 \c
  wire width 4 input 5 \d
  wire width 4 output 6 \y
  wire $n29
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  cell $eq $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'000
    connect \Y $n29
  end
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 3'001
    connect \Y $n35
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 4
    connect \A { $n39 $n38 $n37 $n36 }
    connect \B \a
    connect \S $n29
    connect \Y \y
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 4
    connect \A { $n43 $n42 $n41 $n40 }
    connect \B \b
    connect \S $n35
    connect \Y { $n51 $n50 $n49 $n48 }
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 4
    connect \A { $n51 $n50 $n49 $n48 }
    connect \B 4'xxxx
    connect \S $n29
    connect \Y { $n39 $n38 $n37 $n36 }
  end
  attribute \full_case 1
  cell $mux $c16
    parameter \WIDTH 4
    connect \A \d
    connect \B \c
    connect \S \s [2]
    connect \Y { $n60 $n59 $n58 $n57 }
  end
  attribute \full_case 1
  cell $mux $c17
    parameter \WIDTH 4
    connect \A { $n60 $n59 $n58 $n57 }
    connect \B 4'xxxx
    connect \S $n35
    connect \Y { $n64 $n63 $n62 $n61 }
  end
  attribute \full_case 1
  cell $mux $c18
    parameter \WIDTH 4
    connect \A { $n64 $n63 $n62 $n61 }
    connect \B 4'xxxx
    connect \S $n29
    connect \Y { $n43 $n42 $n41 $n40 }
  end
end
