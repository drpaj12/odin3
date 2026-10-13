# generated: elab-check --write
attribute \top 1
module \e11_conditional
  wire input 1 \s
  wire input 2 \t
  wire width 4 input 3 \a
  wire width 4 input 4 \b
  wire width 4 input 5 \c
  wire width 4 output 6 \y1
  wire width 4 output 7 \y2
  wire width 4 output 8 \y3
  wire width 4 output 9 \y4
  wire width 4 output 10 \y5
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  cell $mux $c11
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y \y1
  end
  cell $mux $c12
    parameter \WIDTH 4
    connect \A \c
    connect \B \b
    connect \S \t
    connect \Y { $n38 $n37 $n36 $n35 }
  end
  cell $mux $c13
    parameter \WIDTH 4
    connect \A { $n38 $n37 $n36 $n35 }
    connect \B \a
    connect \S \s
    connect \Y \y2
  end
  cell $mux $c14
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \t
    connect \Y { $n42 $n41 $n40 $n39 }
  end
  cell $mux $c15
    parameter \WIDTH 4
    connect \A \c
    connect \B { $n42 $n41 $n40 $n39 }
    connect \S \s
    connect \Y \y3
  end
  cell $add $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \b
    connect \B \c
    connect \Y { $n46 $n45 $n44 $n43 }
  end
  cell $mux $c17
    parameter \WIDTH 4
    connect \A { $n46 $n45 $n44 $n43 }
    connect \B \a
    connect \S \s
    connect \Y \y4
  end
  cell $logic_or $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B \t
    connect \Y $n47
  end
  cell $mux $c19
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S $n47
    connect \Y \y5
  end
end
