# generated: elab-check --write
attribute \top 1
module \t20_null
  wire input 1 \a
  wire input 2 \b
  wire width 2 input 3 \s
  wire output 4 \y
  wire output 5 \z
  wire $n7
  wire $n8
  wire $n11
  wire $n12
  process $proc$T20_null_statement.v:3$8
    assign $n7 $n8
    switch \a
      case 1'1
        assign $n8 1'0
      case
        assign $n8 \b
    end
    sync always
      update \y $n7
  end
  process $proc$T20_null_statement.v:11$10
    assign $n11 $n12
    switch \s
      case 2'00
        assign $n12 1'x
      case
        assign $n12 \a
    end
    sync always
      update \z $n11
  end
end
