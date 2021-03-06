package org.opendaylight.opflex.genie.engine.model;
import java.util.Collection;
import java.util.LinkedList;

import org.opendaylight.opflex.modlan.report.Severity;

/**
 * Relator is a special model item object
 * that represents relationship between two
 * or more model entities. It works in conjunction
 * with relator category (@RelatorCat, a special
 * refined implementation of model category).
 *
 * Created by midvorki on 3/27/14.
 */
public class Relator extends Item
{
    public Relator getFromRelator()
    {
        switch (getType())
        {
            case TO:

                return ((Relator) getParent());

            case FROM:
            default:

                return this;
        }
    }

    public Relator getToRelator()
    {
        //System.out.println(this + ".getToRelator()");
        if (Cardinality.SINGLE == getCardinality())
        {
            switch (getType())
            {
                case TO:

                    return this;

                case FROM:
                default:
                    Children lChildren = getNode().getChildren();
                    //System.out.println(this + ".getToRelator(): children:" + lChildren);
                    if (null != lChildren)
                    {
                        //System.out.println(this + ".getToRelator(): children:" + lChildren.getList());

                        CatEntry lCatEntry = lChildren.getEntry(complCat); // HERE'S THE PROBLEM....
                        if (null != lCatEntry)
                        {
                            Collection<Node> lNodes = lCatEntry.getList();
                            switch (lNodes.size())
                            {
                                case 1:
                                {
                                    return (Relator) lNodes.iterator().next().getItem();
                                }

                                case 0:

                                    Severity.DEATH.report(
                                            this.toString(),
                                            "to-relator retrieval",
                                            "data not found",
                                            "node " + getNode() + " 0 instances of children with category " + invItemCat);
                                    break;

                                default:

                                    Severity.DEATH.report(
                                            this.toString(),
                                            "to-relator retrieval",
                                            "ambiguous data",
                                            "node " + getNode() + " too many instances of children with category " +
                                            invItemCat + ":" + lNodes);

                                    break;
                            }

                        }
                        else
                        {
                            Severity.DEATH.report(
                                    this.toString(),
                                    "to-relator retrieval",
                                    "data not found",
                                    "node " + getNode() + " has no children (null entry) with category " + invItemCat +
                                    "; getMap() : " +
                                    lChildren.getMap().keySet() +
                                    " children: " + lChildren);
                        }
                    }
                    else
                    {
                        Severity.DEATH.report(
                                this.toString(),
                                "to-relator retrieval",
                                "data not found",
                                "node " + getNode() + " has no children");
                    }
            }
        }
        else
        {
            Severity.DEATH.report(
                    this.toString(),
                    "to-relator retrieval",
                    "cardinality is multi",
                    "unavaible for cardinality of multi");
        }
        return null;
    }

    /**
     * Same as @getToNode. For a relationship that has singularity of singleton,
     * find the destination/target of the relationship. If this
     * relationship is not Singular, an exception is thrown.
     *
     * @return item that corresponds to resolved node..
     */
    public Item getToItem()
    {
        Relator lRel = getToRelator();
        return null == lRel ? null : lRel.getTargetItem();
    }

    public String getToItemGName()
    {
        Relator lRel = getToRelator();
        return null == lRel ? null : lRel.itemGName;
    }

    /**
     * For any relationship (singular or multi),
     * find the set of destination/target item nodes of this relationship.
     *
     * @return a collection of item objects that are the destination/target of this relationship.
     */
    public Collection<Item> getToItems()
    {
        LinkedList<Item> lRet = new LinkedList<>();
        getToItems(lRet);
        return lRet;
    }


    public boolean hasTo()
    {
        switch (getType())
        {
            case TO:

                return true;

            case FROM:
            default:

                return hasChildren();
        }
    }

    public void getToItems(Collection<Item> aOut)
    {
        Node lStartNode;
        switch (getType())
        {
            case TO:

                lStartNode = getNode().getParent();

                break;

            case FROM:
            default:
            {
                lStartNode = getNode();

            }
        }
        Collection<Node> lNodes = lStartNode.getChildren().getEntry(complCat).getList();
        for (Node lThisNode : lNodes)
        {
            Item lThatItem = ((Relator)lThisNode.getItem()).getTargetItem();
            if (null != lThatItem)
            {
                aOut.add(lThatItem);
            }
            else
            {
                Severity.DEATH.report(this.toString(),"relationship: retrieval of to item", "target item unresolvable", "target: " + itemGName);
            }
        }
    }


    /**
     * get the node that corresponds to this relator instance.
     * @return node that corresponds to this relator instance.
     */
    public Node getTargetNode()
    {
        return itemCat.getNodes().get(itemGName);
    }

    /**
     * get the item that corresponds to this relator instance.
     * @return item that corresponds to this relator instance.
     */
    public Item getTargetItem()
    {
        return itemCat.getNodes().getItem(itemGName);
    }

    /**
     * Accessor of the relator category for this relator
     * @return category of this relator
     */
    public RelatorCat getRelatorCat()
    {
        return (RelatorCat) getNode().getCat();
    }

    /**
     * cardinality accessor. this information is derived form the category object.
     * @return cardinality of the relationship presented by this relator
     */
    public Cardinality getCardinality()
    {
        return getRelatorCat().getCardinality();
    }

    /**
     * type accessor. this information is derived form the category object.
     * @return type identifying from/two nature of this relator
     */
    public RelatorCat.Type getType()
    {
        return getRelatorCat().getType();
    }

    /**
     * Stringifier
     * @return debug string for this Relator
     */
    public String toString()
    {
        StringBuilder lSb = new StringBuilder();
        lSb.append("item:rel");
        lSb.append('(');
        toIdentString(lSb);
        lSb.append(')');
        return lSb.toString();
    }

    /**
     * formatter for the debug identity string
     * @param aInSb string builder into which identity information is recorded.
     */
    public void toIdentString(StringBuilder aInSb)
    {
        if (RelatorCat.Type.TO == getType())
        {
            aInSb.append(getFromRelator().getItemGName());
            aInSb.append("->");
        }
        aInSb.append(itemGName);
        if (null != getNode())
        {
            if (RelatorCat.Type.TO == getType())
            {
                aInSb.append("::");
            }
            else
            {
                aInSb.append("->");
            }
            aInSb.append(getCat().getName());
            aInSb.append('[');
            aInSb.append(getRelatorCat().getCardinality());
            aInSb.append(':');
            aInSb.append(getRelatorCat().getDirection());
            aInSb.append(':');
            aInSb.append(getRelatorCat().getType());
            aInSb.append(']');
        }
    }

    /**
     * validation of the resolvability of the target object
     */
    public void validateCb()
    {
        super.validateCb();
        Node lTargetNode = getTargetNode();
        if (null == lTargetNode)
        {
            Severity.DEATH.report(this.toString(), "validate", "unresolvable target", itemGName + " in cat " + itemCat +
                                 "::: " + itemCat.getNodes().getList());
        }
    }

    /**
     * Constructor
     * @param aInCat model category of the relationship
     * @param aInParent parent relator (in case of TO)
     * @param aInLName relator name
     * @param aInItemCat category of item pointed from
     * @param aInInvItemCat category of item pointed to
     * @param aInItemGName global name of the object pointed by
     */
    public Relator(
            RelatorCat aInCat,
            RelatorCat aInComplCat,
            Relator aInParent,
            String aInLName,
            Cat aInItemCat,
            Cat aInInvItemCat,
            String aInItemGName)
    {
        super(aInCat, aInParent, aInLName);
        complCat = aInComplCat;
        itemCat = aInItemCat;
        invItemCat = aInInvItemCat;
        itemGName = aInItemGName;

        if (Cat.isValidated())
        {
            validateCb();
        }
    }

    public String getItemGName()
    {
        return itemGName;
    }

    private final Cat itemCat;
    private final Cat invItemCat;
    private final RelatorCat complCat;
    private final String itemGName;
}
