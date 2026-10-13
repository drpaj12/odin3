# generated: elab-check --write
attribute \top 1
module \e8_compare
  wire width 4 input 1 \a
  wire width 8 input 2 \b
  wire input 3 \c
  wire output 4 \y1
  wire output 5 \y2
  wire output 6 \y3
  wire output 7 \y4
  wire output 8 \y5
  wire output 9 \y6
  wire output 10 \y7
  wire output 11 \y8
  wire output 12 \y9
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  cell $lt $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y1
  end
  cell $gt $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y2
  end
  cell $le $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y3
  end
  cell $ge $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y4
  end
  cell $eq $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y5
  end
  cell $ne $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \y6
  end
  cell $lt $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n23
  end
  cell $eq $c20
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n23
    connect \B \c
    connect \Y \y7
  end
  cell $eq $c21
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y $n24
  end
  cell $ne $c22
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n24
    connect \B \c
    connect \Y \y8
  end
  cell $add $c23
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y { $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
  end
  cell $lt $c24
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A { $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
    connect \B \c
    connect \Y \y9
  end
end
