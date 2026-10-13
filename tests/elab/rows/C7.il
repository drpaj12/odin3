# generated: elab-check --write
attribute \top 1
module \c7_default_no_colon
  wire width 2 input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire output 4 \y
  wire output 5 \z
  wire $n7
  wire $n8
  wire $n10
  wire $n11
  process $proc$C7_default_without_colon.v:3$7
    assign $n7 $n8
    switch \s
      case 2'00
        assign $n8 \a
      case
        assign $n8 \b
    end
    sync always
      update \y $n7
  end
  process $proc$C7_default_without_colon.v:8$9
    assign $n10 $n11
    switch \s
      case 2'01
        assign $n11 \a
      case
        assign $n11 1'0
    end
    sync always
      update \z $n10
  end
end
